////////////////////////////////////////////////////////////////////////////////
/// @brief replication initial data synchroniser
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "InitialSyncer.h"

#include "Basics/Exceptions.h"
#include "Basics/json.h"
#include "Basics/JsonHelper.h"
#include "Basics/logging.h"
#include "Basics/ReadLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/tri-strings.h"
#include "Indexes/Index.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "Utils/CollectionGuard.h"
#include "Utils/transactions.h"
#include "VocBase/document-collection.h"
#include "VocBase/vocbase.h"
#include "VocBase/voc-types.h"

using namespace std;
using namespace triagens::basics;
using namespace triagens::arango;
using namespace triagens::httpclient;
using namespace triagens::rest;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

size_t const InitialSyncer::MaxChunkSize = 10 * 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

InitialSyncer::InitialSyncer (TRI_vocbase_t* vocbase,
                              TRI_replication_applier_configuration_t const* configuration,
                              std::unordered_map<string, bool> const& restrictCollections,
                              string const& restrictType,
                              bool verbose) :
  Syncer(vocbase, configuration),
  _progress("not started"),
  _restrictCollections(restrictCollections),
  _restrictType(restrictType),
  _processedCollections(),
  _batchId(0),
  _batchUpdateTime(0),
  _batchTtl(180),
  _includeSystem(false),
  _chunkSize(configuration->_chunkSize),
  _verbose(verbose),
  _hasFlushed(false) {

  if (_chunkSize == 0) {
    _chunkSize = (uint64_t) 2 * 1024 * 1024; // 2 mb
  }
  else if (_chunkSize < 128 * 1024) {
    _chunkSize = 128 * 1024;
  }

  _includeSystem = configuration->_includeSystem;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

InitialSyncer::~InitialSyncer () {
  if (_batchId > 0) {
    sendFinishBatch();
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief run method, performs a full synchronisation
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::run (string& errorMsg) {
  if (_client == nullptr || 
      _connection == nullptr || 
      _endpoint == nullptr) {
    errorMsg = "invalid endpoint";

    return TRI_ERROR_INTERNAL;
  }

  setProgress("fetching master state");

  int res = getMasterState(errorMsg);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  res = sendStartBatch(errorMsg);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }


  string url = BaseUrl + "/inventory?serverId=" + _localServerIdString;
  if (_includeSystem) {
    url += "&includeSystem=true";
  }

  // send request
  string const progress = "fetching master inventory from " + url;
  setProgress(progress);

  map<string, string> headers;
  std::unique_ptr<SimpleHttpResult> response(_client->request(HttpRequest::HTTP_REQUEST_GET,
                                                              url,
                                                              nullptr,
                                                              0,
                                                              headers));

  if (response == nullptr || ! response->isComplete()) {
    errorMsg = "could not connect to master at " + string(_masterInfo._endpoint) +
               ": " + _client->getErrorMessage();

    sendFinishBatch();

    return TRI_ERROR_REPLICATION_NO_RESPONSE;
  }

  TRI_ASSERT(response != nullptr);

  if (response->wasHttpError()) {
    res = TRI_ERROR_REPLICATION_MASTER_ERROR;

    errorMsg = "got invalid response from master at " + string(_masterInfo._endpoint) +
               ": HTTP " + StringUtils::itoa(response->getHttpReturnCode()) +
               ": " + response->getHttpReturnMessage();
  }
  else {
    std::unique_ptr<TRI_json_t> json(TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, response->getBody().c_str()));

    if (JsonHelper::isObject(json.get())) {
      res = handleInventoryResponse(json.get(), errorMsg);
    }
    else {
      res = TRI_ERROR_REPLICATION_INVALID_RESPONSE;

      errorMsg = "got invalid response from master at " + string(_masterInfo._endpoint) +
        ": invalid JSON";
    }
  }

  sendFinishBatch();

  return res;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief send a "start batch" command
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::sendStartBatch (string& errorMsg) {
  _batchId = 0;

  string const url = BaseUrl + "/batch";
  string const body = "{\"ttl\":" + StringUtils::itoa(_batchTtl) + "}";

  // send request
  string const progress = "send batch start command to url " + url;
  setProgress(progress);

  map<string, string> const headers;
  std::unique_ptr<SimpleHttpResult> response(_client->request(HttpRequest::HTTP_REQUEST_POST,
                                                              url,
                                                              body.c_str(),
                                                              body.size(),
                                                              headers));

  if (response == nullptr || ! response->isComplete()) {
    errorMsg = "could not connect to master at " + string(_masterInfo._endpoint) +
               ": " + _client->getErrorMessage();

    return TRI_ERROR_REPLICATION_NO_RESPONSE;
  }

  TRI_ASSERT(response != nullptr);

  int res = TRI_ERROR_NO_ERROR;

  if (response->wasHttpError()) {
    res = TRI_ERROR_REPLICATION_MASTER_ERROR;

    errorMsg = "got invalid response from master at " + string(_masterInfo._endpoint) +
               ": HTTP " + StringUtils::itoa(response->getHttpReturnCode()) +
               ": " + response->getHttpReturnMessage();
  }

  if (res == TRI_ERROR_NO_ERROR) {
    std::unique_ptr<TRI_json_t> json(TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, response->getBody().c_str()));

    if (json == nullptr) {
      res = TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }
    else {
      string const id = JsonHelper::getStringValue(json.get(), "id", "");

      if (id.empty()) {
        res = TRI_ERROR_REPLICATION_INVALID_RESPONSE;
      }
      else {
        _batchId = StringUtils::uint64(id);
        _batchUpdateTime = TRI_microtime();
      }
    }
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief send an "extend batch" command
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::sendExtendBatch () {
  if (_batchId == 0) {
    return TRI_ERROR_NO_ERROR;
  }

  double now = TRI_microtime();

  if (now <= _batchUpdateTime + _batchTtl - 60) {
    // no need to extend the batch yet
    return TRI_ERROR_NO_ERROR;
  }

  string const url = BaseUrl + "/batch/" + StringUtils::itoa(_batchId);
  string const body = "{\"ttl\":" + StringUtils::itoa(_batchTtl) + "}";

  // send request
  string const progress = "send batch start command to url " + url;
  setProgress(progress);

  map<string, string> const headers;
  std::unique_ptr<SimpleHttpResult> response(_client->request(HttpRequest::HTTP_REQUEST_PUT,
                                                              url,
                                                              body.c_str(),
                                                              body.size(),
                                                              headers));

  if (response == nullptr || ! response->isComplete()) {
    return TRI_ERROR_REPLICATION_NO_RESPONSE;
  }

  TRI_ASSERT(response != nullptr);

  int res = TRI_ERROR_NO_ERROR;

  if (response->wasHttpError()) {
    res = TRI_ERROR_REPLICATION_MASTER_ERROR;
  }
  else {
    _batchUpdateTime = TRI_microtime();
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief send a "finish batch" command
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::sendFinishBatch () {
  if (_batchId == 0) {
    return TRI_ERROR_NO_ERROR;
  }

  string const url = BaseUrl + "/batch/" + StringUtils::itoa(_batchId);

  // send request
  string const progress = "send batch finish command to url " + url;
  setProgress(progress);

  map<string, string> const headers;
  std::unique_ptr<SimpleHttpResult> response(_client->request(HttpRequest::HTTP_REQUEST_DELETE,
                                                              url,
                                                              nullptr,
                                                              0,
                                                              headers));

  if (response == nullptr || ! response->isComplete()) {
    return TRI_ERROR_REPLICATION_NO_RESPONSE;
  }

  TRI_ASSERT(response != nullptr);

  int res = TRI_ERROR_NO_ERROR;

  if (response->wasHttpError()) {
    res = TRI_ERROR_REPLICATION_MASTER_ERROR;
  }
  else {
    _batchId = 0;
    _batchUpdateTime = 0;
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief apply the data from a collection dump
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::applyCollectionDump (TRI_transaction_collection_t* trxCollection,
                                        SimpleHttpResult* response,
                                        string& errorMsg) {

  const string invalidMsg = "received invalid JSON data for collection " +
                            StringUtils::itoa(trxCollection->_cid);

  StringBuffer& data = response->getBody();
  char* p = data.begin(); 
  char* end = p + data.length();

  // buffer must end with a NUL byte
  TRI_ASSERT(*end == '\0');

  while (p < end) {
    char* q = strchr(p, '\n');

    if (q == nullptr) {
      q = end;
    }
    
    if (q - p < 2) {
      // we are done
      return TRI_ERROR_NO_ERROR;
    }

    TRI_ASSERT(q <= end);
    *q = '\0';

    std::unique_ptr<TRI_json_t> json(TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, p));
    
    p = q + 1;

    if (! JsonHelper::isObject(json.get())) {
      errorMsg = invalidMsg;

      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }

    TRI_replication_operation_e type = REPLICATION_INVALID;
    char const* key       = nullptr;
    TRI_json_t const* doc = nullptr;
    TRI_voc_rid_t rid     = 0;

    auto objects = &(json.get()->_value._objects);
    size_t const n = TRI_LengthVector(objects);

    for (size_t i = 0; i < n; i += 2) {
      auto element = static_cast<TRI_json_t const*>(TRI_AtVector(objects, i));

      if (! JsonHelper::isString(element)) {
        errorMsg = invalidMsg;

        return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
      }

      char const* attributeName = element->_value._string.data;
      auto value = static_cast<TRI_json_t const*>(TRI_AtVector(objects, i + 1));

      if (TRI_EqualString(attributeName, "type")) {
        if (JsonHelper::isNumber(value)) {
          type = (TRI_replication_operation_e) (int) value->_value._number;
        }
      }

      else if (TRI_EqualString(attributeName, "key")) {
        if (JsonHelper::isString(value)) {
          key = value->_value._string.data;
        }
      }

      else if (TRI_EqualString(attributeName, "rev")) {
        if (JsonHelper::isString(value)) {
          rid = StringUtils::uint64(value->_value._string.data, value->_value._string.length - 1);
        }
      }

      else if (TRI_EqualString(attributeName, "data")) {
        if (JsonHelper::isObject(value)) {
          doc = value;
        }
      }
    }

    // key must not be 0, but doc can be 0!
    if (key == nullptr) {
      errorMsg = invalidMsg;

      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }

    int res = applyCollectionDumpMarker(trxCollection, type, (const TRI_voc_key_t) key, rid, doc, errorMsg);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  // reached the end      
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief incrementally fetch data from a collection
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::handleCollectionDump (string const& cid,
                                         TRI_transaction_collection_t* trxCollection,
                                         string const& collectionName,
                                         TRI_voc_tick_t maxTick,
                                         string& errorMsg) {

  std::string appendix;

  if (_hasFlushed) {
    appendix = "&flush=false";
  }
  else {
    // only flush WAL once
    appendix = "&flush=true";
    _hasFlushed = true;
  }

  uint64_t chunkSize = _chunkSize;

  string const baseUrl = BaseUrl +
                         "/dump?collection=" + cid +
                         appendix;

  map<string, string> headers;

  TRI_voc_tick_t fromTick = 0;
  int batch = 1;

  while (true) {
    sendExtendBatch();

    string url = baseUrl + "&from=" + StringUtils::itoa(fromTick);

    if (maxTick > 0) {
      url += "&to=" + StringUtils::itoa(maxTick);
    }

    url += "&serverId=" + _localServerIdString;
    url += "&chunkSize=" + StringUtils::itoa(chunkSize);
  
    std::string const typeString = (trxCollection->_collection->_collection->_info._type == TRI_COL_TYPE_EDGE ? "edge" : "document");

    // send request
    string const progress = "fetching master collection dump for collection '" + collectionName +
                            "', type: " + typeString + ", id " + cid + ", batch " + StringUtils::itoa(batch);

    setProgress(progress.c_str());

    std::unique_ptr<SimpleHttpResult> response(_client->request(HttpRequest::HTTP_REQUEST_GET,
                                                                url,
                                                                nullptr,
                                                                0,
                                                                headers));

    if (response == nullptr || ! response->isComplete()) {
      errorMsg = "could not connect to master at " + string(_masterInfo._endpoint) +
                 ": " + _client->getErrorMessage();

      return TRI_ERROR_REPLICATION_NO_RESPONSE;
    }

    TRI_ASSERT(response != nullptr);

    if (response->wasHttpError()) {
      errorMsg = "got invalid response from master at " + string(_masterInfo._endpoint) +
                 ": HTTP " + StringUtils::itoa(response->getHttpReturnCode()) +
                 ": " + response->getHttpReturnMessage();

      return TRI_ERROR_REPLICATION_MASTER_ERROR;
    }

    int res = TRI_ERROR_NO_ERROR;  // Just to please the compiler
    bool checkMore = false;
    bool found;
    TRI_voc_tick_t tick;

    string header = response->getHeaderField(TRI_REPLICATION_HEADER_CHECKMORE, found);
    if (found) {
      checkMore = StringUtils::boolean(header);
      res = TRI_ERROR_NO_ERROR;

      if (checkMore) {
        header = response->getHeaderField(TRI_REPLICATION_HEADER_LASTINCLUDED, found);
        if (found) {
          tick = StringUtils::uint64(header);

          if (tick > fromTick) {
            fromTick = tick;
          }
          else {
            // we got the same tick again, this indicates we're at the end
            checkMore = false;
          }
        }
      }
    }

    if (! found) {
      errorMsg = "got invalid response from master at " + string(_masterInfo._endpoint) +
                 ": required header is missing";
      res = TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }

    if (res == TRI_ERROR_NO_ERROR) {
      res = applyCollectionDump(trxCollection, response.get(), errorMsg);
    }

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    if (! checkMore || fromTick == 0) {
      // done
      return res;
    }

    // increase chunk size for next fetch
    if (chunkSize < MaxChunkSize) {
      chunkSize = static_cast<uint64_t>(chunkSize * 1.5);
      if (chunkSize > MaxChunkSize) {
        chunkSize = MaxChunkSize;
      }
    }

    batch++;
  }

  TRI_ASSERT(false);
  return TRI_ERROR_INTERNAL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief handle the information about a collection
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::handleCollection (TRI_json_t const* parameters,
                                     TRI_json_t const* indexes,
                                     string& errorMsg,
                                     sync_phase_e phase) {

  sendExtendBatch();

  string const masterName = JsonHelper::getStringValue(parameters, "name", "");

  TRI_json_t const* masterId = JsonHelper::getObjectElement(parameters, "cid");

  if (! JsonHelper::isString(masterId)) {
    errorMsg = "collection id is missing in response";

    return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
  }
  
  TRI_json_t const* type = JsonHelper::getObjectElement(parameters, "type");

  if (! JsonHelper::isNumber(type)) {
    errorMsg = "collection type is missing in response";
    
    return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
  }

  std::string const typeString = (type->_value._number == 3 ? "edge" : "document");

  TRI_voc_cid_t const cid = StringUtils::uint64(masterId->_value._string.data, masterId->_value._string.length - 1);
  string const collectionMsg = "collection '" + masterName + "', type " + typeString + ", id " + StringUtils::itoa(cid);


  // phase handling
  if (phase == PHASE_VALIDATE) {
    // validation phase just returns ok if we got here (aborts above if data is invalid)
    _processedCollections.emplace(cid, masterName);

    return TRI_ERROR_NO_ERROR;
  }

  // drop collections locally
  // -------------------------------------------------------------------------------------

  if (phase == PHASE_DROP) {
    // first look up the collection by the cid
    TRI_vocbase_col_t* col = TRI_LookupCollectionByIdVocBase(_vocbase, cid);

    if (col == nullptr && ! masterName.empty()) {
      // not found, try name next
      col = TRI_LookupCollectionByNameVocBase(_vocbase, masterName.c_str());
    }

    if (col != nullptr) {
      bool truncate = false;

      if (col->_name[0] == '_' && 
          TRI_EqualString(col->_name, TRI_COL_NAME_USERS)) {
        // better not throw away the _users collection. otherwise it is gone and this may be a problem if the
        // server crashes in-between.
        truncate = true;
      }

      if (truncate) {
        // system collection
        setProgress("truncating " + collectionMsg);
     
        SingleCollectionWriteTransaction<UINT64_MAX> trx(new StandaloneTransactionContext(), _vocbase, col->_cid);

        int res = trx.begin();

        if (res != TRI_ERROR_NO_ERROR) {
          errorMsg = "unable to truncate " + collectionMsg + ": " + TRI_errno_string(res);
 
          return res;
        }

        res = trx.truncate(false);
 
        if (res != TRI_ERROR_NO_ERROR) {
          errorMsg = "unable to truncate " + collectionMsg + ": " + TRI_errno_string(res);
 
          return res;
        }

        res = trx.commit();
        
        if (res != TRI_ERROR_NO_ERROR) {
          errorMsg = "unable to truncate " + collectionMsg + ": " + TRI_errno_string(res);
 
          return res;
        }
      }
      else {
        // regular collection
        setProgress("dropping " + collectionMsg);
      
        int res = TRI_DropCollectionVocBase(_vocbase, col, true);

        if (res != TRI_ERROR_NO_ERROR) {
          errorMsg = "unable to drop " + collectionMsg + ": " + TRI_errno_string(res);

          return res;
        }
      }
    }

    return TRI_ERROR_NO_ERROR;
  }

  // re-create collections locally
  // -------------------------------------------------------------------------------------

  else if (phase == PHASE_CREATE) {
    TRI_vocbase_col_t* col = nullptr;

    string const progress = "creating " + collectionMsg;
    setProgress(progress.c_str());

    int res = createCollection(parameters, &col);

    if (res != TRI_ERROR_NO_ERROR) {
      errorMsg = "unable to create " + collectionMsg + ": " + TRI_errno_string(res);

      return res;
    }

    return TRI_ERROR_NO_ERROR;
  }

  // sync collection data
  // -------------------------------------------------------------------------------------

  else if (phase == PHASE_DUMP) {
    string const progress = "syncing data for " + collectionMsg;
    setProgress(progress.c_str());
    
    TRI_vocbase_col_t* col = TRI_LookupCollectionByIdVocBase(_vocbase, cid);

    if (col == nullptr && ! masterName.empty()) {
      // not found, try name next
      col = TRI_LookupCollectionByNameVocBase(_vocbase, masterName.c_str());
    }

    if (col == nullptr) {
      errorMsg = "cannot dump: " + collectionMsg + " not found";

      return TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND;
    }

    int res = TRI_ERROR_INTERNAL;

    {
      SingleCollectionWriteTransaction<UINT64_MAX> trx(new StandaloneTransactionContext(), _vocbase, col->_cid);

      res = trx.begin();

      if (res != TRI_ERROR_NO_ERROR) {
        errorMsg = "unable to start transaction: " + string(TRI_errno_string(res));

        return res;
      }

      TRI_transaction_collection_t* trxCollection = trx.trxCollection();

      if (trxCollection == nullptr) {
        res = TRI_ERROR_INTERNAL;
        errorMsg = "unable to start transaction: " + string(TRI_errno_string(res));
      }
      else {
        res = handleCollectionDump(StringUtils::itoa(cid), trxCollection, masterName, _masterInfo._lastLogTick, errorMsg);
      }

      res = trx.finish(res);
    }

    if (res == TRI_ERROR_NO_ERROR) {
      // now create indexes
      size_t const n = TRI_LengthVector(&indexes->_value._objects);

      if (n > 0) {
        string const progress = "creating indexes for " + collectionMsg;
        setProgress(progress.c_str());

        READ_LOCKER(_vocbase->_inventoryLock);

        try {
          triagens::arango::CollectionGuard guard(_vocbase, col->_cid, false);
          TRI_vocbase_col_t* col = guard.collection();

          if (col == nullptr) {
            res = TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND;
          }
          else {
            TRI_document_collection_t* document = col->_collection;
            TRI_ASSERT(document != nullptr);

            // create a fake transaction object to avoid assertions
            TransactionBase trx(true);
            TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(document);

            for (size_t i = 0; i < n; ++i) {
              TRI_json_t const* idxDef = static_cast<TRI_json_t const*>(TRI_AtVector(&indexes->_value._objects, i));
              triagens::arango::Index* idx = nullptr;
 
              // {"id":"229907440927234","type":"hash","unique":false,"fields":["x","Y"]}
    
              res = TRI_FromJsonIndexDocumentCollection(document, idxDef, &idx);

              if (res != TRI_ERROR_NO_ERROR) {
                errorMsg = "could not create index: " + string(TRI_errno_string(res));
                break;
              }
              else {
                TRI_ASSERT(idx != nullptr);

                res = TRI_SaveIndex(document, idx, true);

                if (res != TRI_ERROR_NO_ERROR) {
                  errorMsg = "could not save index: " + string(TRI_errno_string(res));
                  break;
                }
              }
            }

            TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(document);
          }
        }
        catch (triagens::basics::Exception const& ex) {
          res = ex.code();
        }
        catch (...) {
          res = TRI_ERROR_INTERNAL;
        }

      }
    }

    return res;
  }


  // we won't get here
  TRI_ASSERT(false);
  return TRI_ERROR_INTERNAL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief handle the inventory response of the master
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::handleInventoryResponse (TRI_json_t const* json,
                                            string& errorMsg) {
  TRI_json_t const* data = JsonHelper::getObjectElement(json, "collections");

  if (! JsonHelper::isArray(data)) {
    errorMsg = "collections section is missing from response";

    return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
  }
  
  std::vector<std::pair<TRI_json_t const*, TRI_json_t const*>> collections;
  size_t const n = TRI_LengthVector(&data->_value._objects);
  
  for (size_t i = 0; i < n; ++i) {
    auto collection = static_cast<TRI_json_t const*>(TRI_AtVector(&data->_value._objects, i));

    if (! JsonHelper::isObject(collection)) {
      errorMsg = "collection declaration is invalid in response";

      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }
    
    TRI_json_t const* parameters = JsonHelper::getObjectElement(collection, "parameters");

    if (! JsonHelper::isObject(parameters)) {
      errorMsg = "collection parameters declaration is invalid in response";

      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }
    
    TRI_json_t const* indexes = JsonHelper::getObjectElement(collection, "indexes");

    if (! JsonHelper::isArray(indexes)) {
      errorMsg = "collection indexes declaration is invalid in response";

      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }

    string const masterName = JsonHelper::getStringValue(parameters, "name", "");
  
    if (masterName.empty()) {
      errorMsg = "collection name is missing in response";

      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }
  
    if (TRI_ExcludeCollectionReplication(masterName.c_str(), _includeSystem)) {
      continue;
    }
  
    if (JsonHelper::getBooleanValue(parameters, "deleted", false)) {
      // we don't care about deleted collections
      continue;
    }
  
    if (! _restrictType.empty()) {
      auto const it = _restrictCollections.find(masterName);

      bool found = (it != _restrictCollections.end());

      if (_restrictType == "include" && ! found) {
        // collection should not be included
        continue;
      }
      else if (_restrictType == "exclude" && found) {
        // collection should be excluded
        continue;
      }
    }

    collections.emplace_back(std::make_pair(parameters, indexes));
  }

  int res;

  // STEP 1: validate collection declarations from master
  // ----------------------------------------------------------------------------------

  // iterate over all collections from the master...
  res = iterateCollections(collections, errorMsg, PHASE_VALIDATE);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }


  // STEP 2: drop collections locally if they are also present on the master (clean up)
  // ----------------------------------------------------------------------------------

  res = iterateCollections(collections, errorMsg, PHASE_DROP);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }


  // STEP 3: re-create empty collections locally
  // ----------------------------------------------------------------------------------

  res = iterateCollections(collections, errorMsg, PHASE_CREATE);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }


  // STEP 4: sync collection data from master and create initial indexes
  // ----------------------------------------------------------------------------------

  return iterateCollections(collections, errorMsg, PHASE_DUMP);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief iterate over all collections from an array and apply an action
////////////////////////////////////////////////////////////////////////////////

int InitialSyncer::iterateCollections (std::vector<std::pair<TRI_json_t const*, TRI_json_t const*>> const& collections,
                                       string& errorMsg,
                                       sync_phase_e phase) {
  std::string phaseMsg("starting phase " + translatePhase(phase) + " with " + std::to_string(collections.size()) + " collections");
  setProgress(phaseMsg); 

  for (auto const& collection : collections) {
    TRI_json_t const* parameters = collection.first;
    TRI_json_t const* indexes    = collection.second;

    TRI_ASSERT(parameters != nullptr);
    TRI_ASSERT(indexes != nullptr);

    int res = handleCollection(parameters, indexes, errorMsg, phase);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  // all ok
  return TRI_ERROR_NO_ERROR;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
