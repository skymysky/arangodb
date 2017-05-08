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
/// @author Jan Steemann
/// @author Daniel H. Larkin
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBEngine/RocksDBKey.h"
#include "Basics/Exceptions.h"
#include "Logger/Logger.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBTypes.h"

using namespace arangodb;
using namespace arangodb::rocksutils;

const char RocksDBKey::_stringSeparator = '\0';

RocksDBKey RocksDBKey::Database(TRI_voc_tick_t databaseId) {
  return RocksDBKey(RocksDBEntryType::Database, databaseId);
}

RocksDBKey RocksDBKey::Collection(TRI_voc_tick_t databaseId,
                                  TRI_voc_cid_t collectionId) {
  return RocksDBKey(RocksDBEntryType::Collection, databaseId, collectionId);
}

RocksDBKey RocksDBKey::Document(uint64_t collectionId,
                                TRI_voc_rid_t revisionId) {
  return RocksDBKey(RocksDBEntryType::Document, collectionId, revisionId);
}

RocksDBKey RocksDBKey::PrimaryIndexValue(
    uint64_t indexId, arangodb::StringRef const& primaryKey) {
  return RocksDBKey(RocksDBEntryType::PrimaryIndexValue, indexId, primaryKey);
}

RocksDBKey RocksDBKey::PrimaryIndexValue(uint64_t indexId,
                                         char const* primaryKey) {
  return RocksDBKey(RocksDBEntryType::PrimaryIndexValue, indexId,
                    StringRef(primaryKey));
}

RocksDBKey RocksDBKey::EdgeIndexValue(uint64_t indexId,
                                      std::string const& vertexId,
                                      std::string const& primaryKey) {
  return RocksDBKey(RocksDBEntryType::EdgeIndexValue, indexId, vertexId,
                    primaryKey);
}

RocksDBKey RocksDBKey::IndexValue(uint64_t indexId,
                                  arangodb::StringRef const& primaryKey,
                                  VPackSlice const& indexValues) {
  return RocksDBKey(RocksDBEntryType::IndexValue, indexId, primaryKey,
                    indexValues);
}

RocksDBKey RocksDBKey::UniqueIndexValue(uint64_t indexId,
                                        VPackSlice const& indexValues) {
  return RocksDBKey(RocksDBEntryType::UniqueIndexValue, indexId, indexValues);
}

RocksDBKey GeoIndexValue(uint64_t indexId, bool isSlot, uint64_t offset){
  RocksDBKey(RocksDBEntryType::GeoIndexValue, isSlot, offset);
}

RocksDBKey RocksDBKey::View(TRI_voc_tick_t databaseId, TRI_voc_cid_t viewId) {
  return RocksDBKey(RocksDBEntryType::View, databaseId, viewId);
}

RocksDBKey RocksDBKey::CounterValue(uint64_t objectId) {
  return RocksDBKey(RocksDBEntryType::CounterValue, objectId);
}

RocksDBKey RocksDBKey::SettingsValue() {
  return RocksDBKey(RocksDBEntryType::SettingsValue);
}

RocksDBKey RocksDBKey::ReplicationApplierConfig(TRI_voc_tick_t databaseId) {
  return RocksDBKey(RocksDBEntryType::ReplicationApplierConfig, databaseId);
}

RocksDBEntryType RocksDBKey::type(RocksDBKey const& key) {
  return type(key._buffer.data(), key._buffer.size());
}

RocksDBEntryType RocksDBKey::type(rocksdb::Slice const& slice) {
  return type(slice.data(), slice.size());
}

uint64_t RocksDBKey::counterObjectId(rocksdb::Slice const& s) {
  TRI_ASSERT(s.size() >= (sizeof(char) + sizeof(uint64_t)));
  return uint64FromPersistent(s.data() + sizeof(char));
}

TRI_voc_tick_t RocksDBKey::databaseId(RocksDBKey const& key) {
  return databaseId(key._buffer.data(), key._buffer.size());
}

TRI_voc_tick_t RocksDBKey::databaseId(rocksdb::Slice const& slice) {
  return databaseId(slice.data(), slice.size());
}

TRI_voc_cid_t RocksDBKey::collectionId(RocksDBKey const& key) {
  return collectionId(key._buffer.data(), key._buffer.size());
}

TRI_voc_cid_t RocksDBKey::collectionId(rocksdb::Slice const& slice) {
  return collectionId(slice.data(), slice.size());
}

uint64_t RocksDBKey::objectId(RocksDBKey const& key) {
  return objectId(key._buffer.data(), key._buffer.size());
}
uint64_t RocksDBKey::objectId(rocksdb::Slice const& slice) {
  return objectId(slice.data(), slice.size());
}

TRI_voc_cid_t RocksDBKey::viewId(RocksDBKey const& key) {
  return viewId(key._buffer.data(), key._buffer.size());
}

TRI_voc_cid_t RocksDBKey::viewId(rocksdb::Slice const& slice) {
  return viewId(slice.data(), slice.size());
}

TRI_voc_rid_t RocksDBKey::revisionId(RocksDBKey const& key) {
  return revisionId(key._buffer.data(), key._buffer.size());
}

TRI_voc_rid_t RocksDBKey::revisionId(rocksdb::Slice const& slice) {
  return revisionId(slice.data(), slice.size());
}

arangodb::StringRef RocksDBKey::primaryKey(RocksDBKey const& key) {
  return primaryKey(key._buffer.data(), key._buffer.size());
}

arangodb::StringRef RocksDBKey::primaryKey(rocksdb::Slice const& slice) {
  return primaryKey(slice.data(), slice.size());
}

std::string RocksDBKey::vertexId(RocksDBKey const& key) {
  return vertexId(key._buffer.data(), key._buffer.size());
}

std::string RocksDBKey::vertexId(rocksdb::Slice const& slice) {
  return vertexId(slice.data(), slice.size());
}

VPackSlice RocksDBKey::indexedVPack(RocksDBKey const& key) {
  return indexedVPack(key._buffer.data(), key._buffer.size());
}

VPackSlice RocksDBKey::indexedVPack(rocksdb::Slice const& slice) {
  return indexedVPack(slice.data(), slice.size());
}

std::string const& RocksDBKey::string() const { return _buffer; }

RocksDBKey::RocksDBKey(RocksDBEntryType type) : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::SettingsValue: {
      _buffer.push_back(static_cast<char>(_type));
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, uint64_t first)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::Database:
    case RocksDBEntryType::CounterValue:
    case RocksDBEntryType::ReplicationApplierConfig: {
      size_t length = sizeof(char) + sizeof(uint64_t);
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      uint64ToPersistent(_buffer, first);  // databaseId
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, uint64_t first,
                       VPackSlice const& slice)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::UniqueIndexValue: {
      size_t length = sizeof(char) + sizeof(uint64_t) +
                      static_cast<size_t>(slice.byteSize()) + sizeof(char);
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      uint64ToPersistent(_buffer, first);
      _buffer.append(reinterpret_cast<char const*>(slice.begin()),
                     static_cast<size_t>(slice.byteSize()));
      _buffer.push_back(_stringSeparator);

      TRI_ASSERT(_buffer.size() == length);
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, uint64_t first, uint64_t second)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::Collection:
    case RocksDBEntryType::Document:
    case RocksDBEntryType::View: {
      size_t length = sizeof(char) + (2 * sizeof(uint64_t));
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      uint64ToPersistent(_buffer,
                         first);  // databaseId / collectionId / databaseId
      uint64ToPersistent(_buffer,
                         second);  // collectionId / revisionId / viewId
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, uint64_t first,
                       arangodb::StringRef const& docKey,
                       VPackSlice const& indexData)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::IndexValue: {
      // Non-unique VPack index values are stored as follows:
      // - Key: 6 + 8-byte object ID of index + VPack array with index value(s)
      // + separator byte + primary key + primary key length
      // - Value: empty
      size_t length = sizeof(char) + sizeof(uint64_t) +
                      static_cast<size_t>(indexData.byteSize()) + sizeof(char) +
                      docKey.length() + sizeof(char);
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      uint64ToPersistent(_buffer, first);
      _buffer.append(reinterpret_cast<char const*>(indexData.begin()),
                     static_cast<size_t>(indexData.byteSize()));
      _buffer.push_back(_stringSeparator);
      _buffer.append(docKey.data(), docKey.length());
      _buffer.push_back(static_cast<char>(docKey.length() & 0xff));

      TRI_ASSERT(_buffer.size() == length);
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, uint64_t first,
                       arangodb::StringRef const& second)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::PrimaryIndexValue: {
      size_t length = sizeof(char) + sizeof(uint64_t) + second.size();
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      uint64ToPersistent(_buffer, first);
      _buffer.append(second.data(), second.size());
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, bool isSlot, uint64_t offset)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::GeoIndexValue: {

      size_t length = sizeof(char) + sizeof(isSlot) + sizeof(offset);
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      _buffer.append(isSlot, sizeof(isSlot));
      uint64ToPersistent(_buffer, offset);
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBKey::RocksDBKey(RocksDBEntryType type, uint64_t first,
                       std::string const& second, std::string const& third)
    : _type(type), _buffer() {
  switch (_type) {
    case RocksDBEntryType::EdgeIndexValue: {
      size_t length = sizeof(char) + sizeof(uint64_t) + second.size() +
                      sizeof(char) + third.size() + sizeof(uint8_t);
      _buffer.reserve(length);
      _buffer.push_back(static_cast<char>(_type));
      uint64ToPersistent(_buffer, first);
      _buffer.append(second);
      _buffer.push_back(_stringSeparator);
      _buffer.append(third);
      TRI_ASSERT(third.size() <= 254);
      _buffer.push_back(static_cast<char>(third.size() & 0xff));
      break;
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }
}

RocksDBEntryType RocksDBKey::type(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  return static_cast<RocksDBEntryType>(data[0]);
}

TRI_voc_tick_t RocksDBKey::databaseId(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::Database:
    case RocksDBEntryType::Collection:
    case RocksDBEntryType::View:
    case RocksDBEntryType::ReplicationApplierConfig: {
      TRI_ASSERT(size >= (sizeof(char) + sizeof(uint64_t)));
      return uint64FromPersistent(data + sizeof(char));
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

TRI_voc_cid_t RocksDBKey::collectionId(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::Collection: {
      TRI_ASSERT(size >= (sizeof(char) + (2 * sizeof(uint64_t))));
      return uint64FromPersistent(data + sizeof(char) + sizeof(uint64_t));
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

TRI_voc_cid_t RocksDBKey::objectId(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::Document:
    case RocksDBEntryType::PrimaryIndexValue:
    case RocksDBEntryType::EdgeIndexValue:
    case RocksDBEntryType::IndexValue:
    case RocksDBEntryType::UniqueIndexValue:
    case RocksDBEntryType::GeoIndexValue:
    {
      TRI_ASSERT(size >= (sizeof(char) + (2 * sizeof(uint64_t))));
      return uint64FromPersistent(data + sizeof(char));
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

TRI_voc_cid_t RocksDBKey::viewId(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::View: {
      TRI_ASSERT(size >= (sizeof(char) + (2 * sizeof(uint64_t))));
      return uint64FromPersistent(data + sizeof(char) + sizeof(uint64_t));
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

TRI_voc_rid_t RocksDBKey::revisionId(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::Document: {
      TRI_ASSERT(size >= (sizeof(char) + (2 * sizeof(uint64_t))));
      return uint64FromPersistent(data + sizeof(char) + sizeof(uint64_t));
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

arangodb::StringRef RocksDBKey::primaryKey(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::PrimaryIndexValue: {
      TRI_ASSERT(size >= (sizeof(char) + sizeof(uint64_t) + sizeof(char)));
      size_t keySize = size - (sizeof(char) + sizeof(uint64_t));
      return arangodb::StringRef(data + sizeof(char) + sizeof(uint64_t),
                                 keySize);
    }
    case RocksDBEntryType::EdgeIndexValue:
    case RocksDBEntryType::IndexValue: {
      TRI_ASSERT(size > (sizeof(char) + sizeof(uint64_t) + sizeof(uint8_t)));
      size_t keySize = static_cast<size_t>(data[size - 1]);
      return arangodb::StringRef(data + (size - (keySize + sizeof(uint8_t))),
                                 keySize);
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

std::string RocksDBKey::vertexId(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::EdgeIndexValue: {
      TRI_ASSERT(size > (sizeof(char) + sizeof(uint64_t) + sizeof(uint8_t)));
      size_t keySize = static_cast<size_t>(data[size - 1]);
      size_t idSize = size - (sizeof(char) + sizeof(uint64_t) + sizeof(char) +
                              keySize + sizeof(uint8_t));
      return std::string(data + sizeof(char) + sizeof(uint64_t), idSize);
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}

VPackSlice RocksDBKey::indexedVPack(char const* data, size_t size) {
  TRI_ASSERT(data != nullptr);
  TRI_ASSERT(size >= sizeof(char));
  RocksDBEntryType type = static_cast<RocksDBEntryType>(data[0]);
  switch (type) {
    case RocksDBEntryType::IndexValue:
    case RocksDBEntryType::UniqueIndexValue: {
      TRI_ASSERT(size > (sizeof(char) + sizeof(uint64_t)));
      return VPackSlice(data + sizeof(char) + sizeof(uint64_t));
    }

    default:
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TYPE_ERROR);
  }
}
