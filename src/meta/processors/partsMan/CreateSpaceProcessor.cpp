/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "meta/processors/partsMan/CreateSpaceProcessor.h"
#include "meta/ActiveHostsMan.h"

DEFINE_int32(default_parts_num, 100, "The default number of parts when a space is created");
DEFINE_int32(default_replica_factor, 1, "The default replica factor when a space is created");

namespace nebula {
namespace meta {

const std::string defaultGroup = "default";         // NOLINT

void CreateSpaceProcessor::process(const cpp2::CreateSpaceReq& req) {
    folly::SharedMutex::WriteHolder wHolder(LockUtils::spaceLock());
    auto properties = req.get_properties();
    auto spaceRet = getSpaceId(properties.get_space_name());
    if (spaceRet.ok()) {
        cpp2::ErrorCode ret;
        if (req.get_if_not_exists()) {
            ret = cpp2::ErrorCode::SUCCEEDED;
        } else {
            LOG(ERROR) << "Create Space Failed : Space " << properties.get_space_name()
                       << " have existed!";
            ret = cpp2::ErrorCode::E_EXISTED;
        }

        resp_.set_id(to(spaceRet.value(), EntryType::SPACE));
        handleErrorCode(ret);
        onFinished();
        return;
    }

    CHECK_EQ(Status::SpaceNotFound(), spaceRet.status());
    auto spaceName = properties.get_space_name();
    auto partitionNum = properties.get_partition_num();
    auto replicaFactor = properties.get_replica_factor();
    auto charsetName = properties.get_charset_name();
    auto collateName = properties.get_collate_name();
    auto& vid = properties.get_vid_type();
    auto vidSize = vid.__isset.type_length ? *vid.get_type_length() : 0;
    auto vidType = vid.get_type();

    // Use default values or values from meta's configuration file
    if (partitionNum == 0) {
        partitionNum = FLAGS_default_parts_num;
        if (partitionNum <= 0) {
            LOG(ERROR) << "Create Space Failed : partition_num is illegal: " << partitionNum;
            handleErrorCode(cpp2::ErrorCode::E_INVALID_PARM);
            onFinished();
            return;
        }
        // Set the default value back to the struct, which will be written to storage
        properties.set_partition_num(partitionNum);
    }
    if (replicaFactor == 0) {
        replicaFactor = FLAGS_default_replica_factor;
        if (replicaFactor <= 0) {
            LOG(ERROR) << "Create Space Failed : replicaFactor is illegal: " << replicaFactor;
            resp_.set_code(cpp2::ErrorCode::E_INVALID_PARM);
            onFinished();
            return;
        }
        // Set the default value back to the struct, which will be written to storage
        properties.set_replica_factor(replicaFactor);
    }
    if (vidSize == 0) {
        LOG(ERROR) << "Create Space Failed : vid_size is illegal: " << vidSize;
        handleErrorCode(cpp2::ErrorCode::E_INVALID_PARM);
        onFinished();
        return;
    }
    if (vidType != cpp2::PropertyType::INT64 && vidType != cpp2::PropertyType::FIXED_STRING) {
        LOG(ERROR) << "Create Space Failed : vid_type is illegal: "
                   << meta::cpp2::_PropertyType_VALUES_TO_NAMES.at(vidType);
        handleErrorCode(cpp2::ErrorCode::E_INVALID_PARM);
        onFinished();
        return;
    }
    if (vidType == cpp2::PropertyType::INT64 && vidSize != 8) {
        LOG(ERROR) << "Create Space Failed : vid_size should be 8 if vid type is interger: "
                   << vidSize;
        handleErrorCode(cpp2::ErrorCode::E_INVALID_PARM);
        onFinished();
        return;
    }

    properties.vid_type.set_type_length(vidSize);
    auto idRet = autoIncrementId();
    if (!nebula::ok(idRet)) {
        LOG(ERROR) << "Create Space Failed : Get space id failed";
        handleErrorCode(nebula::error(idRet));
        onFinished();
        return;
    }

    auto spaceId = nebula::value(idRet);
    std::vector<kvstore::KV> data;
    data.emplace_back(MetaServiceUtils::indexSpaceKey(spaceName),
                      std::string(reinterpret_cast<const char*>(&spaceId), sizeof(spaceId)));
    data.emplace_back(MetaServiceUtils::spaceKey(spaceId),
                      MetaServiceUtils::spaceVal(properties));

    if (properties.__isset.group_name) {
        std::string* groupName = properties.get_group_name();
        LOG(INFO) << "Create Space on group: " << *groupName;
        auto groupKey = MetaServiceUtils::groupKey(*groupName);
        auto ret = doGet(groupKey);
        if (!ret.ok()) {
            LOG(ERROR) << "Group Name: " << *groupName << " not found";
            handleErrorCode(cpp2::ErrorCode::E_NOT_FOUND);
            onFinished();
            return;
        }

        auto zones = MetaServiceUtils::parseZoneNames(ret.value());
        int32_t zoneNum = zones.size();
        if (replicaFactor > zoneNum) {
            LOG(ERROR) << "Replication number should less than or equal to zone number";
            handleErrorCode(cpp2::ErrorCode::E_INVALID_PARM);
            onFinished();
            return;
        }

        for (auto partId = 1; partId <= partitionNum; partId++) {
            auto partHosts = pickHostsWithZone(partId, zones, replicaFactor);
            data.emplace_back(MetaServiceUtils::partKey(spaceId, partId),
                              MetaServiceUtils::partVal(partHosts));
        }
    } else {
        auto hosts = ActiveHostsMan::getActiveHosts(kvstore_);
        if (hosts.empty()) {
            LOG(ERROR) << "Create Space Failed : No Hosts!";
            handleErrorCode(cpp2::ErrorCode::E_NO_HOSTS);
            onFinished();
            return;
        }

        if ((int32_t)hosts.size() < replicaFactor) {
            LOG(ERROR) << "Not enough hosts existed for replica "
                       << replicaFactor << ", hosts num " << hosts.size();
            handleErrorCode(cpp2::ErrorCode::E_UNSUPPORTED);
            onFinished();
            return;
        }

        for (auto partId = 1; partId <= partitionNum; partId++) {
            auto partHosts = pickHosts(partId, hosts, replicaFactor);
            data.emplace_back(MetaServiceUtils::partKey(spaceId, partId),
                              MetaServiceUtils::partVal(partHosts));
        }
    }

    handleErrorCode(cpp2::ErrorCode::SUCCEEDED);
    resp_.set_id(to(spaceId, EntryType::SPACE));
    doSyncPutAndUpdate(std::move(data));
    LOG(INFO) << "Create space " << spaceName;
}


std::vector<HostAddr>
CreateSpaceProcessor::pickHosts(PartitionID partId,
                                const std::vector<HostAddr>& hosts,
                                int32_t replicaFactor) {
    auto startIndex = partId;
    std::vector<HostAddr> pickedHosts;
    for (int32_t i = 0; i < replicaFactor; i++) {
        pickedHosts.emplace_back(toThriftHost(hosts[startIndex++ % hosts.size()]));
    }
    return pickedHosts;
}

std::vector<HostAddr>
CreateSpaceProcessor::pickHostsWithZone(PartitionID partId,
                                        const std::vector<std::string>& zones,
                                        int32_t replicaFactor) {
    auto startIndex = partId;
    std::vector<HostAddr> pickedHosts;
    for (int32_t i = 0; i < replicaFactor; i++) {
        auto zoneName = zones[startIndex++ % zones.size()];
        auto zoneIdRet = getZoneId(zoneName);
        if (!zoneIdRet.ok()) {
            LOG(ERROR) << "Zone " << zoneName << " not found";
            handleErrorCode(cpp2::ErrorCode::E_NOT_FOUND);
            onFinished();
            return pickedHosts;
        }

        auto zoneKey = MetaServiceUtils::zoneKey(zoneName);
        auto zoneValueRet = doGet(std::move(zoneKey));
        if (!zoneValueRet.ok()) {
            LOG(ERROR) << "Get zone " << zoneName << " failed";
            handleErrorCode(cpp2::ErrorCode::E_STORE_FAILURE);
            onFinished();
            return pickedHosts;
        }
        auto hosts = MetaServiceUtils::parseZoneHosts(std::move(zoneValueRet).value());
        pickedHosts.emplace_back(toThriftHost(hosts[startIndex++ % hosts.size()]));
    }
    return pickedHosts;
}
}  // namespace meta
}  // namespace nebula
