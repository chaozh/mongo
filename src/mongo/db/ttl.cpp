/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/ttl.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/ttl_gen.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangTTLMonitorWithLock);

Counter64 ttlPasses;
Counter64 ttlDeletedDocuments;

ServerStatusMetricField<Counter64> ttlPassesDisplay("ttl.passes", &ttlPasses);
ServerStatusMetricField<Counter64> ttlDeletedDocumentsDisplay("ttl.deletedDocuments",
                                                              &ttlDeletedDocuments);

class TTLMonitor : public BackgroundJob {
public:
    TTLMonitor(ServiceContext* serviceContext) : _serviceContext(serviceContext) {}
    virtual ~TTLMonitor() {}

    virtual std::string name() const {
        return "TTLMonitor";
    }

    static std::string secondsExpireField;

    virtual void run() {
        ThreadClient tc(name(), _serviceContext);
        AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationKillable(lk);
        }

        while (!globalInShutdownDeprecated()) {
            {
                MONGO_IDLE_THREAD_BLOCK;
                sleepsecs(ttlMonitorSleepSecs.load());
            }

            LOG(3) << "thread awake";

            if (!ttlMonitorEnabled.load()) {
                LOG(1) << "disabled";
                continue;
            }

            if (lockedForWriting()) {
                // Note: this is not perfect as you can go into fsync+lock between this and actually
                // doing the delete later.
                LOG(3) << "locked for writing";
                continue;
            }

            try {
                doTTLPass();
            } catch (const WriteConflictException&) {
                LOG(1) << "got WriteConflictException";
            } catch (const ExceptionForCat<ErrorCategory::Interruption>& interruption) {
                LOG(1) << "TTLMonitor was interrupted: " << interruption;
            }
        }
    }

private:
    void doTTLPass() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        // If part of replSet but not in a readable state (e.g. during initial sync), skip.
        if (repl::ReplicationCoordinator::get(&opCtx)->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet &&
            !repl::ReplicationCoordinator::get(&opCtx)->getMemberState().readable())
            return;

        TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());
        std::vector<std::pair<UUID, std::string>> ttlInfos = ttlCollectionCache.getTTLInfos();

        // Pair of collection namespace and index spec.
        std::vector<std::pair<NamespaceString, BSONObj>> ttlIndexes;

        ttlPasses.increment();

        // Get all TTL indexes from every collection.
        for (const std::pair<UUID, std::string>& ttlInfo : ttlInfos) {
            auto uuid = ttlInfo.first;
            auto indexName = ttlInfo.second;

            auto nss = CollectionCatalog::get(opCtxPtr.get()).lookupNSSByUUID(&opCtx, uuid);
            if (!nss) {
                ttlCollectionCache.deregisterTTLInfo(ttlInfo);
                continue;
            }

            AutoGetCollection autoColl(&opCtx, *nss, MODE_IS);
            Collection* coll = autoColl.getCollection();
            // The collection with `uuid` might be renamed before the lock and the wrong
            // namespace would be locked and looked up so we double check here.
            if (!coll || coll->uuid() != uuid)
                continue;

            if (!DurableCatalog::get(opCtxPtr.get())
                     ->isIndexPresent(&opCtx, coll->getCatalogId(), indexName)) {
                ttlCollectionCache.deregisterTTLInfo(ttlInfo);
                continue;
            }

            BSONObj spec = DurableCatalog::get(opCtxPtr.get())
                               ->getIndexSpec(&opCtx, coll->getCatalogId(), indexName);
            if (!spec.hasField(secondsExpireField)) {
                ttlCollectionCache.deregisterTTLInfo(ttlInfo);
                continue;
            }

            if (!DurableCatalog::get(opCtxPtr.get())
                     ->isIndexReady(&opCtx, coll->getCatalogId(), indexName))
                continue;

            ttlIndexes.push_back(std::make_pair(*nss, spec.getOwned()));
        }

        for (const auto& it : ttlIndexes) {
            try {
                doTTLForIndex(&opCtx, it.first, it.second);
            } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
                warning() << "TTLMonitor was interrupted, waiting " << ttlMonitorSleepSecs.load()
                          << " seconds before doing another pass";
                return;
            } catch (const DBException& dbex) {
                error() << "Error processing ttl index: " << it.second << " -- " << dbex.toString();
                // Continue on to the next index.
                continue;
            }
        }
    }

    /**
     * Remove documents from the collection using the specified TTL index after a sufficient amount
     * of time has passed according to its expiry specification.
     */
    void doTTLForIndex(OperationContext* opCtx, NamespaceString collectionNSS, BSONObj idx) {
        if (collectionNSS.isDropPendingNamespace()) {
            return;
        }
        if (!userAllowedWriteNS(collectionNSS).isOK()) {
            error() << "namespace '" << collectionNSS
                    << "' doesn't allow deletes, skipping ttl job for: " << idx;
            return;
        }

        const BSONObj key = idx["key"].Obj();
        const StringData name = idx["name"].valueStringData();
        if (key.nFields() != 1) {
            error() << "key for ttl index can only have 1 field, skipping ttl job for: " << idx;
            return;
        }

        LOG(1) << "ns: " << collectionNSS << " key: " << key << " name: " << name;

        AutoGetCollection autoGetCollection(opCtx, collectionNSS, MODE_IX);
        if (MONGO_unlikely(hangTTLMonitorWithLock.shouldFail())) {
            log() << "Hanging due to hangTTLMonitorWithLock fail point";
            hangTTLMonitorWithLock.pauseWhileSet(opCtx);
        }


        Collection* collection = autoGetCollection.getCollection();
        if (!collection) {
            // Collection was dropped.
            return;
        }

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionNSS)) {
            return;
        }

        const IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName(opCtx, name);
        if (!desc) {
            LOG(1) << "index not found (index build in progress? index dropped?), skipping "
                   << "ttl job for: " << idx;
            return;
        }

        // Re-read 'idx' from the descriptor, in case the collection or index definition changed
        // before we re-acquired the collection lock.
        idx = desc->infoObj();

        if (IndexType::INDEX_BTREE != IndexNames::nameToType(desc->getAccessMethodName())) {
            error() << "special index can't be used as a ttl index, skipping ttl job for: " << idx;
            return;
        }

        BSONElement secondsExpireElt = idx[secondsExpireField];
        if (!secondsExpireElt.isNumber()) {
            error() << "ttl indexes require the " << secondsExpireField << " field to be "
                    << "numeric but received a type of " << typeName(secondsExpireElt.type())
                    << ", skipping ttl job for: " << idx;
            return;
        }

        const Date_t kDawnOfTime =
            Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
        const Date_t expirationTime = Date_t::now() - Seconds(secondsExpireElt.numberLong());
        const BSONObj startKey = BSON("" << kDawnOfTime);
        const BSONObj endKey = BSON("" << expirationTime);
        // The canonical check as to whether a key pattern element is "ascending" or
        // "descending" is (elt.number() >= 0).  This is defined by the Ordering class.
        const InternalPlanner::Direction direction = (key.firstElement().number() >= 0)
            ? InternalPlanner::Direction::FORWARD
            : InternalPlanner::Direction::BACKWARD;

        // We need to pass into the DeleteStageParams (below) a CanonicalQuery with a BSONObj that
        // queries for the expired documents correctly so that we do not delete documents that are
        // not actually expired when our snapshot changes during deletion.
        const char* keyFieldName = key.firstElement().fieldName();
        BSONObj query =
            BSON(keyFieldName << BSON("$gte" << kDawnOfTime << "$lte" << expirationTime));
        auto qr = std::make_unique<QueryRequest>(collectionNSS);
        qr->setFilter(query);
        auto canonicalQuery = CanonicalQuery::canonicalize(opCtx, std::move(qr));
        invariant(canonicalQuery.getStatus());

        auto params = std::make_unique<DeleteStageParams>();
        params->isMulti = true;
        params->canonicalQuery = canonicalQuery.getValue().get();

        auto exec =
            InternalPlanner::deleteWithIndexScan(opCtx,
                                                 collection,
                                                 std::move(params),
                                                 desc,
                                                 startKey,
                                                 endKey,
                                                 BoundInclusion::kIncludeBothStartAndEndKeys,
                                                 PlanExecutor::YIELD_AUTO,
                                                 direction);

        Status result = exec->executePlan();
        if (!result.isOK()) {
            error() << "ttl query execution for index " << idx
                    << " failed with status: " << redact(result);
            return;
        }

        const long long numDeleted = DeleteStage::getNumDeleted(*exec);
        ttlDeletedDocuments.increment(numDeleted);
        LOG(1) << "deleted: " << numDeleted;
    }

    ServiceContext* _serviceContext;
};

namespace {
// The global TTLMonitor object is intentionally leaked.  Even though it is only used in one
// function, we declare it here to indicate to the leak sanitizer that the leak of this object
// should not be reported.
TTLMonitor* ttlMonitor = nullptr;
}  // namespace

void startTTLBackgroundJob(ServiceContext* serviceContext) {
    ttlMonitor = new TTLMonitor(serviceContext);
    ttlMonitor->go();
}

std::string TTLMonitor::secondsExpireField = "expireAfterSeconds";
}  // namespace mongo
