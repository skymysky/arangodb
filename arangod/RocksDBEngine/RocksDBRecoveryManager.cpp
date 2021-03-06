////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
/// @author Daniel Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBRecoveryManager.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Logger/Logger.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamily.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBCuckooIndexEstimator.h"
#include "RocksDBEngine/RocksDBEdgeIndex.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBKeyBounds.h"
#include "RocksDBEngine/RocksDBRecoveryHelper.h"
#include "RocksDBEngine/RocksDBSettingsManager.h"
#include "RocksDBEngine/RocksDBVPackIndex.h"
#include "RocksDBEngine/RocksDBValue.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/ticks.h"

#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>
#include <rocksdb/write_batch.h>

#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::application_features;

RocksDBRecoveryManager* RocksDBRecoveryManager::instance() {
  return ApplicationServer::getFeature<RocksDBRecoveryManager>(featureName());
}

/// Constructor needs to be called synchrunously,
/// will load counts from the db and scan the WAL
RocksDBRecoveryManager::RocksDBRecoveryManager(
    application_features::ApplicationServer* server)
    : ApplicationFeature(server, featureName()),
      _db(nullptr),
      _inRecovery(true) {
  setOptional(true);
  requiresElevatedPrivileges(false);
  startsAfter("Database");
  startsAfter("RocksDBEngine");
  startsAfter("StorageEngine");
  startsAfter("ServerId");

  onlyEnabledWith("RocksDBEngine");
}

void RocksDBRecoveryManager::start() {
  if (!isEnabled()) {
    return;
  }

  _db = ApplicationServer::getFeature<RocksDBEngine>("RocksDBEngine")->db();
  runRecovery();
  _inRecovery = false;

  // notify everyone that recovery is now done
  auto databaseFeature =
      ApplicationServer::getFeature<DatabaseFeature>("Database");
  databaseFeature->recoveryDone();
}

/// parse recent RocksDB WAL entries and notify the
/// DatabaseFeature about the successful recovery
void RocksDBRecoveryManager::runRecovery() {
  bool success = parseRocksWAL();
  if (!success) {
  }  // TODO what do we do if not successful?
}

bool RocksDBRecoveryManager::inRecovery() const { return _inRecovery; }

class WBReader final : public rocksdb::WriteBatch::Handler {
 public:
  std::unordered_map<uint64_t, RocksDBSettingsManager::CounterAdjustment>
      deltas;
  rocksdb::SequenceNumber currentSeqNum;

 private:
  // must be retrieved from settings manager
  std::unordered_map<uint64_t, rocksdb::SequenceNumber> _seqStart;
  std::unordered_map<uint64_t, uint64_t> _generators;

  uint64_t _maxTick = 0;
  uint64_t _maxHLC = 0;

 public:
  explicit WBReader(std::unordered_map<uint64_t, rocksdb::SequenceNumber> seqs)
      : currentSeqNum(0), _seqStart(seqs) {}

  ~WBReader() {
    // update ticks after parsing wal
    LOG_TOPIC(TRACE, Logger::ENGINES) << "max tick found in WAL: " << _maxTick
                                      << ", last HLC value: " << _maxHLC;

    TRI_UpdateTickServer(_maxTick);
    TRI_HybridLogicalClock(_maxHLC);

    // TODO update generators
    auto dbfeature = ApplicationServer::getFeature<DatabaseFeature>("Database");
    for (auto gen : _generators) {
      if (gen.second > 0) {
        auto dbColPair = rocksutils::mapObjectToCollection(gen.first);
        if (dbColPair.second == 0 && dbColPair.first == 0) {
          // collection with this objectID not known.Skip.
          continue;
        }
        auto vocbase = dbfeature->useDatabase(dbColPair.first);
        if (vocbase == nullptr) {
          continue;
        }
        TRI_DEFER(vocbase->release());

        auto collection = vocbase->lookupCollection(dbColPair.second);
        if (collection == nullptr) {
          continue;
        }
        std::string k(basics::StringUtils::itoa(gen.second));
        collection->keyGenerator()->track(k.data(), k.size());
      }
    }
  }

  bool shouldHandleDocument(uint32_t column_family_id,
                            const rocksdb::Slice& key) {
    if (column_family_id == RocksDBColumnFamily::documents()->GetID()) {
      uint64_t objectId = RocksDBKey::objectId(key);
      auto const& it = _seqStart.find(objectId);
      if (it != _seqStart.end()) {
        if (deltas.find(objectId) == deltas.end()) {
          deltas.emplace(objectId, RocksDBSettingsManager::CounterAdjustment());
        }
        return it->second <= currentSeqNum;
      }
    }
    return false;
  }

  void storeMaxHLC(uint64_t hlc) {
    if (hlc > _maxHLC) {
      _maxHLC = hlc;
    }
  }

  void storeMaxTick(uint64_t tick) {
    if (tick > _maxTick) {
      _maxTick = tick;
    }
  }

  void storeLastKeyValue(uint64_t objectId, uint64_t keyValue) {
    if (keyValue == 0) {
      return;
    }

    auto it = _generators.find(objectId);

    if (it == _generators.end()) {
      try {
        _generators.emplace(objectId, keyValue);
      } catch (...) {
      }
      return;
    }

    if (keyValue > (*it).second) {
      (*it).second = keyValue;
    }
  }

  std::pair<RocksDBCuckooIndexEstimator<uint64_t>*, uint64_t> findEstimator(
      uint64_t objectId) {
    RocksDBEngine* engine =
        static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
    Index* index = engine->mapObjectToIndex(objectId);
    if (index == nullptr) {
      return std::make_pair(nullptr, 0);
    }
    return static_cast<RocksDBIndex*>(index)->estimator();
  }

  void updateMaxTick(uint32_t column_family_id, const rocksdb::Slice& key,
                     const rocksdb::Slice& value) {
    // RETURN (side-effect): update _maxTick
    //
    // extract max tick from Markers and store them as side-effect in
    // _maxTick member variable that can be used later (dtor) to call
    // TRI_UpdateTickServer (ticks.h)
    // Markers: - collections (id,objectid) as tick and max tick in indexes
    // array
    //          - documents - _rev (revision as maxtick)
    //          - databases

    if (column_family_id == RocksDBColumnFamily::documents()->GetID()) {
      storeMaxHLC(RocksDBKey::revisionId(RocksDBEntryType::Document, key));
      storeLastKeyValue(RocksDBKey::objectId(key),
                        RocksDBValue::keyValue(value));
    } else if (column_family_id == RocksDBColumnFamily::primary()->GetID()) {
      // document key
      StringRef ref = RocksDBKey::primaryKey(key);
      TRI_ASSERT(!ref.empty());
      // check if the key is numeric
      if (ref[0] >= '1' && ref[0] <= '9') {
        // numeric start byte. looks good
        try {
          // extract uint64_t value from key. this will throw if the key
          // is non-numeric
          uint64_t tick =
              basics::StringUtils::uint64_check(ref.data(), ref.size());
          // if no previous _maxTick set or the numeric value found is
          // "near" our previous _maxTick, then we update it
          if (tick > _maxTick && (_maxTick == 0 || tick - _maxTick < 2048)) {
            storeMaxTick(tick);
          }
        } catch (...) {
          // non-numeric key. simply ignore it
        }
      }
    } else if (column_family_id ==
               RocksDBColumnFamily::definitions()->GetID()) {
      auto const type = RocksDBKey::type(key);

      if (type == RocksDBEntryType::Collection) {
        storeMaxTick(RocksDBKey::collectionId(key));
        auto slice = RocksDBValue::data(value);
        storeMaxTick(basics::VelocyPackHelper::stringUInt64(slice, "objectId"));
        VPackSlice indexes = slice.get("indexes");
        for (VPackSlice const& idx : VPackArrayIterator(indexes)) {
          storeMaxTick(
              std::max(basics::VelocyPackHelper::stringUInt64(idx, "objectId"),
                       basics::VelocyPackHelper::stringUInt64(idx, "id")));
        }
      } else if (type == RocksDBEntryType::Database) {
        storeMaxTick(RocksDBKey::databaseId(key));
      } else if (type == RocksDBEntryType::View) {
        storeMaxTick(
            std::max(RocksDBKey::databaseId(key), RocksDBKey::viewId(key)));
      }
    }
  }

  rocksdb::Status PutCF(uint32_t column_family_id, const rocksdb::Slice& key,
                        const rocksdb::Slice& value) override {
    updateMaxTick(column_family_id, key, value);
    if (shouldHandleDocument(column_family_id, key)) {
      uint64_t objectId = RocksDBKey::objectId(key);
      uint64_t revisionId =
          RocksDBKey::revisionId(RocksDBEntryType::Document, key);

      auto const& it = deltas.find(objectId);
      if (it != deltas.end()) {
        it->second._sequenceNum = currentSeqNum;
        it->second._added++;
        it->second._revisionId = revisionId;
      }
    } else {
      // We have to adjust the estimate with an insert
      uint64_t hash = 0;
      if (column_family_id == RocksDBColumnFamily::vpack()->GetID()) {
        hash = RocksDBVPackIndex::HashForKey(key);
      } else if (column_family_id == RocksDBColumnFamily::edge()->GetID()) {
        hash = RocksDBEdgeIndex::HashForKey(key);
      }

      if (hash != 0) {
        uint64_t objectId = RocksDBKey::objectId(key);
        auto est = findEstimator(objectId);
        if (est.first != nullptr && est.second < currentSeqNum) {
          // We track estimates for this index
          est.first->insert(hash);
        }
      }
    }

    RocksDBEngine* engine =
        static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
    for (auto helper : engine->recoveryHelpers()) {
      helper->PutCF(column_family_id, key, value);
    }

    return rocksdb::Status();
  }

  rocksdb::Status DeleteCF(uint32_t column_family_id,
                           const rocksdb::Slice& key) override {
    if (shouldHandleDocument(column_family_id, key)) {
      uint64_t objectId = RocksDBKey::objectId(key);
      uint64_t revisionId =
          RocksDBKey::revisionId(RocksDBEntryType::Document, key);

      auto const& it = deltas.find(objectId);
      if (it != deltas.end()) {
        it->second._sequenceNum = currentSeqNum;
        it->second._removed++;
        it->second._revisionId = revisionId;
      }
    } else {
      // We have to adjust the estimate with an insert
      uint64_t hash = 0;
      if (column_family_id == RocksDBColumnFamily::vpack()->GetID()) {
        hash = RocksDBVPackIndex::HashForKey(key);
      } else if (column_family_id == RocksDBColumnFamily::edge()->GetID()) {
        hash = RocksDBEdgeIndex::HashForKey(key);
      }

      if (hash != 0) {
        uint64_t objectId = RocksDBKey::objectId(key);
        auto est = findEstimator(objectId);
        if (est.first != nullptr && est.second < currentSeqNum) {
          // We track estimates for this index
          est.first->remove(hash);
        }
      }
    }

    RocksDBEngine* engine =
        static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
    for (auto helper : engine->recoveryHelpers()) {
      helper->DeleteCF(column_family_id, key);
    }

    return rocksdb::Status();
  }

  rocksdb::Status SingleDeleteCF(uint32_t column_family_id,
                                 const rocksdb::Slice& key) override {
    RocksDBEngine* engine =
        static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
    for (auto helper : engine->recoveryHelpers()) {
      helper->SingleDeleteCF(column_family_id, key);
    }

    return rocksdb::Status();
  }

  void LogData(const rocksdb::Slice& blob) override {
    RocksDBEngine* engine =
        static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
    for (auto helper : engine->recoveryHelpers()) {
      helper->LogData(blob);
    }
  }
};

/// parse the WAL with the above handler parser class
bool RocksDBRecoveryManager::parseRocksWAL() {
  RocksDBEngine* engine =
      static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
  for (auto helper : engine->recoveryHelpers()) {
    helper->prepare();
  }

  // Tell the WriteBatch reader the transaction markers to look for
  auto handler =
      std::make_unique<WBReader>(engine->settingsManager()->counterSeqs());

  auto minTick = std::min(engine->settingsManager()->earliestSeqNeeded(),
                          engine->releasedTick());
  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;  // reader();
  rocksdb::Status s = _db->GetUpdatesSince(
      minTick, &iterator, rocksdb::TransactionLogIterator::ReadOptions(true));
  if (!s.ok()) {  // TODO do something?
    return false;
  }

  while (iterator->Valid()) {
    s = iterator->status();
    if (s.ok()) {
      rocksdb::BatchResult batch = iterator->GetBatch();
      handler->currentSeqNum = batch.sequence;
      s = batch.writeBatchPtr->Iterate(handler.get());
    }
    if (!s.ok()) {
      LOG_TOPIC(ERR, Logger::ENGINES) << "error during WAL scan";
      break;
    }

    iterator->Next();
  }

  LOG_TOPIC(TRACE, Logger::ENGINES)
      << "finished WAL scan with " << handler->deltas.size();
  for (std::pair<uint64_t, RocksDBSettingsManager::CounterAdjustment> pair :
       handler->deltas) {
    engine->settingsManager()->updateCounter(pair.first, pair.second);
    LOG_TOPIC(TRACE, Logger::ENGINES)
        << "WAL recovered " << pair.second.added() << " PUTs and "
        << pair.second.removed() << " DELETEs for objectID " << pair.first;
  }
  return handler->deltas.size() > 0;
}
