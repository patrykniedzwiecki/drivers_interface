/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OHOS_HDI_DISPLAY_V1_0_DISPLAY_CMD_REQUESTER_H
#define OHOS_HDI_DISPLAY_V1_0_DISPLAY_CMD_REQUESTER_H

#include <fstream>
#include <poll.h>
#include <securec.h>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>

#include "base/hdi_smq.h"
#include "buffer_handle_utils.h"
#include "command_pack/command_data_packer.h"
#include "command_pack/command_data_unpacker.h"
#include "display_cmd_utils.h"
#include "hdf_base.h"
#include "hdf_trace.h"
#include "hdifd_parcelable.h"
#include "hilog/log.h"
#include "idisplay_composer_vdi.h"
#include "parameter.h"
#include "v1_0/display_composer_type.h"
#include "v1_0/mapper_stub.h"

#define DISPLAY_TRACE HdfTrace trace(__func__, "HDI:DISP:")

namespace OHOS {
namespace HDI {
namespace Display {
namespace Composer {
namespace V1_0 {
using namespace OHOS::HDI::Base;
using namespace OHOS::HDI::Display::Composer::V1_0;
using namespace OHOS::HDI::Display::Buffer::V1_0;
using HdifdSet = std::vector<std::shared_ptr<HdifdParcelable>>;

static constexpr uint32_t TIME_BUFFER_MAX_LEN = 15;

static sptr<IMapper> g_bufferServiceImpl = nullptr;

template <typename Transfer, typename VdiImpl>
class DisplayCmdResponser {
public:
    static std::unique_ptr<DisplayCmdResponser> Create(VdiImpl* impl, std::shared_ptr<DeviceCacheManager> cacheMgr)
    {
        DISPLAY_CHK_RETURN(impl == nullptr, nullptr,
            HDF_LOGE("%{public}s: error, VdiImpl is nullptr", __func__));
        DISPLAY_CHK_RETURN(cacheMgr == nullptr, nullptr,
            HDF_LOGE("%{public}s: error, VdiImpl is nullptr", __func__));
        return std::make_unique<DisplayCmdResponser>(impl, cacheMgr);
    }

    DisplayCmdResponser(VdiImpl* impl, std::shared_ptr<DeviceCacheManager> cacheMgr)
        : impl_(impl),
        cacheMgr_(cacheMgr),
        request_(nullptr),
        isReplyUpdated_(false),
        reply_(nullptr),
        replyCommandCnt_(0),
        replyPacker_(nullptr) {}

    ~DisplayCmdResponser() {}

    int32_t InitCmdRequest(const std::shared_ptr<Transfer>& request)
    {
        DISPLAY_CHK_RETURN(request == nullptr, HDF_FAILURE,
            HDF_LOGE("%{public}s: error, request is nullptr", __func__));
        if (request_ != nullptr) {
            request_.reset();
        }
        request_ = request;

        return HDF_SUCCESS;
    }

    int32_t GetCmdReply(std::shared_ptr<Transfer>& reply)
    {
        int32_t ret = HDF_SUCCESS;
        if (isReplyUpdated_ == false) {
            ret = InitReply(CmdUtils::INIT_ELEMENT_COUNT);
        }
        if (ret == HDF_SUCCESS) {
            if (reply_ != nullptr) {
                reply = reply_;
            } else {
                ret = HDF_FAILURE;
            }
        }
        isReplyUpdated_ = false;
        if (ret != HDF_SUCCESS) {
            HDF_LOGE("error: GetCmdReply failure");
        }

        return ret;
    }

    int32_t ProcessRequestCmd(std::shared_ptr<CommandDataUnpacker> unpacker, int32_t cmd,
        const std::vector<HdifdInfo>& inFds, std::vector<HdifdInfo>& outFds)
    {
        int32_t ret = HDF_SUCCESS;
        HDF_LOGD("%{public}s: PackSection, cmd-[%{public}d] = %{public}s",
            __func__, cmd, CmdUtils::CommandToString(cmd));
        switch (cmd) {
            case REQUEST_CMD_PREPARE_DISPLAY_LAYERS:
                OnPrepareDisplayLayers(unpacker);
                break;
            case REQUEST_CMD_SET_DISPLAY_CLIENT_BUFFER:
                OnSetDisplayClientBuffer(unpacker, inFds);
                break;
            case REQUEST_CMD_SET_DISPLAY_CLIENT_DAMAGE:
                OnSetDisplayClientDamage(unpacker);
                break;
            case REQUEST_CMD_COMMIT:
                OnCommit(unpacker, outFds);
                break;
            case REQUEST_CMD_SET_LAYER_ALPHA:
                OnSetLayerAlpha(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_REGION:
                OnSetLayerRegion(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_CROP:
                OnSetLayerCrop(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_ZORDER:
                OnSetLayerZorder(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_PREMULTI:
                OnSetLayerPreMulti(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_TRANSFORM_MODE:
                OnSetLayerTransformMode(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_DIRTY_REGION:
                OnSetLayerDirtyRegion(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_VISIBLE_REGION:
                OnSetLayerVisibleRegion(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_BUFFER:
                OnSetLayerBuffer(unpacker, inFds);
                break;
            case REQUEST_CMD_SET_LAYER_COMPOSITION_TYPE:
                OnSetLayerCompositionType(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_BLEND_TYPE:
                OnSetLayerBlendType(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_MASK_INFO:
                OnSetLayerMaskInfo(unpacker);
                break;
            case CONTROL_CMD_REQUEST_END:
                ret = OnRequestEnd(unpacker);
                break;
            case REQUEST_CMD_SET_LAYER_COLOR:
                OnSetLayerColor(unpacker);
                break;
            default:
                HDF_LOGE("%{public}s: not support this cmd, unpacked cmd = %{public}d", __func__, cmd);
                ret = HDF_FAILURE;
                break;
        }
        return ret;
    }

    int32_t CmdRequest(uint32_t inEleCnt, const std::vector<HdifdInfo>& inFds, uint32_t& outEleCnt,
        std::vector<HdifdInfo>& outFds)
    {
        std::shared_ptr<char> requestData(new char[inEleCnt * CmdUtils::ELEMENT_SIZE], std::default_delete<char[]>());
        int32_t ret = request_->Read(reinterpret_cast<int32_t *>(requestData.get()), inEleCnt,
            CmdUtils::TRANSFER_WAIT_TIME);

        std::shared_ptr<CommandDataUnpacker> unpacker = std::make_shared<CommandDataUnpacker>();
        DISPLAY_CHK_RETURN(unpacker == nullptr, HDF_FAILURE,
            HDF_LOGE("%{public}s: unpacker construct failed", __func__));

        unpacker->Init(requestData.get(), inEleCnt * CmdUtils::ELEMENT_SIZE);
#ifdef DEBUG_DISPLAY_CMD_RAW_DATA
        unpacker->Dump();
#endif // DEBUG_DISPLAY_CMD_RAW_DATA

        int32_t unpackCmd = -1;
        bool retBool = unpacker->PackBegin(unpackCmd);
        DISPLAY_CHK_RETURN(retBool == false, HDF_FAILURE,
            HDF_LOGE("%{public}s: error: Check RequestBegin failed", __func__));
        DISPLAY_CHK_RETURN(unpackCmd != CONTROL_CMD_REQUEST_BEGIN, HDF_FAILURE,
            HDF_LOGI("error: unpacker PackBegin cmd not match, cmd(%{public}d)=%{public}s.", unpackCmd,
            CmdUtils::CommandToString(unpackCmd)));

        while (ret == HDF_SUCCESS && unpacker->NextSection()) {
            if (!unpacker->BeginSection(unpackCmd)) {
                HDF_LOGE("error: PackSection failed, unpackCmd=%{public}s.",
                    CmdUtils::CommandToString(unpackCmd));
                ret = HDF_FAILURE;
            }
            ret = ProcessRequestCmd(unpacker, unpackCmd, inFds, outFds);
        }

        DISPLAY_CHK_RETURN(ret != HDF_SUCCESS, ret,
            HDF_LOGE("%{public}s: ProcessRequestCmd failed", __func__));
        /* pack request end commands */
        replyPacker_->PackEnd(CONTROL_CMD_REPLY_END);

#ifdef DEBUG_DISPLAY_CMD_RAW_DATA
        /* just for debug */
        replyPacker_->Dump();
        HDF_LOGI("CmdReply command cnt=%{public}d", replyCommandCnt_);
#endif // DEBUG_DISPLAY_CMD_RAW_DATA

        /*  Write reply pack */
        outEleCnt = replyPacker_->ValidSize() / CmdUtils::ELEMENT_SIZE;
        ret = reply_->Write(reinterpret_cast<int32_t *>(replyPacker_->GetDataPtr()), outEleCnt,
            CmdUtils::TRANSFER_WAIT_TIME);
        if (ret != HDF_SUCCESS) {
            HDF_LOGE("Reply write failure, ret=%{public}d", ret);
            outEleCnt = 0;
        }
        int32_t ec = PeriodDataReset();
        return (ret == HDF_SUCCESS ? ec : ret);
    }

private:
    int32_t InitReply(uint32_t size)
    {
        reply_ = std::make_shared<Transfer>(size, SmqType::SYNCED_SMQ);
        DISPLAY_CHK_RETURN(reply_ == nullptr, HDF_FAILURE,
            HDF_LOGE("%{public}s: reply_ construct failed", __func__));

        replyPacker_ = std::make_shared<CommandDataPacker>();
        DISPLAY_CHK_RETURN(replyPacker_ == nullptr, HDF_FAILURE,
            HDF_LOGE("%{public}s: replyPacker_ construct failed", __func__));

        bool retBool = replyPacker_->Init(reply_->GetSize() * CmdUtils::ELEMENT_SIZE);
        DISPLAY_CHK_RETURN(retBool == false, HDF_FAILURE,
            HDF_LOGE("%{public}s: replyPacker_ init failed", __func__));

        return CmdUtils::StartPack(CONTROL_CMD_REPLY_BEGIN, replyPacker_);
    }

    int32_t OnRequestEnd(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        size_t errCnt = errMaps_.size();
        if (errCnt >= 0) {
            int32_t ret = CmdUtils::StartSection(REPLY_CMD_SET_ERROR, replyPacker_);
            DISPLAY_CHK_RETURN(ret != HDF_SUCCESS, ret,
                HDF_LOGE("%{public}s: StartSection failed", __func__));

            bool result = replyPacker_->WriteUint32(errCnt);
            DISPLAY_CHK_RETURN(result == false, HDF_FAILURE,
                HDF_LOGE("%{public}s: write errCnt failed", __func__));
            for (auto it = errMaps_.begin(); it != errMaps_.end(); ++it) {
                result = replyPacker_->WriteInt32(it->first);
                DISPLAY_CHK_RETURN(result == false, HDF_FAILURE,
                    HDF_LOGE("%{public}s: write err-cmd failed, cmdId:%{public}s",
                    __func__, CmdUtils::CommandToString(it->first)));

                result = replyPacker_->WriteInt32(it->second);
                DISPLAY_CHK_RETURN(result == false, HDF_FAILURE,
                    HDF_LOGE("%{public}s: write errNo failed, errNo:%{public}d", __func__, it->second));
            }
            result = CmdUtils::EndSection(replyPacker_);
            DISPLAY_CHK_RETURN(result == false, HDF_FAILURE,
                HDF_LOGE("%{public}s: write replyPacker_ EndSection failed", __func__));
            replyCommandCnt_++;
        }
        return HDF_SUCCESS;
    }

    void OnPrepareDisplayLayers(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        bool needFlush = false;
        uint32_t vectSize = 0;
        std::vector<uint32_t> layers;
        std::vector<int32_t> types;

        int32_t ret = unpacker->ReadUint32(devId) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->PrepareDisplayLayers(devId, needFlush);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->GetDisplayCompChange(devId, layers, types);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = CmdUtils::StartSection(REPLY_CMD_PREPARE_DISPLAY_LAYERS, replyPacker_);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        DISPLAY_CHECK(replyPacker_->WriteUint32(devId) == false, goto EXIT);

        DISPLAY_CHECK(replyPacker_->WriteBool(needFlush) == false, goto EXIT);
        // Write layers vector
        vectSize = static_cast<uint32_t>(layers.size());
        DISPLAY_CHECK(replyPacker_->WriteUint32(vectSize) == false, goto EXIT);

        for (uint32_t i = 0; i < vectSize; i++) {
            DISPLAY_CHECK(replyPacker_->WriteUint32(layers[i]) == false, goto EXIT);
        }
        // Write composer types vector
        vectSize = static_cast<uint32_t>(types.size());
        DISPLAY_CHECK(replyPacker_->WriteUint32(vectSize) == false, goto EXIT);

        for (uint32_t i = 0; i < vectSize; i++) {
            DISPLAY_CHECK(replyPacker_->WriteUint32(types[i]) == false, goto EXIT);
        }
        // End this cmd section
        ret = CmdUtils::EndSection(replyPacker_);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
        replyCommandCnt_++;
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_PREPARE_DISPLAY_LAYERS, ret);
        }
        return;
    }

    void OnSetDisplayClientBuffer(std::shared_ptr<CommandDataUnpacker> unpacker, const std::vector<HdifdInfo>& inFds)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        int32_t ret = unpacker->ReadUint32(devId) ? HDF_SUCCESS : HDF_FAILURE;

        BufferHandle *buffer = nullptr;
        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::BufferHandleUnpack(unpacker, inFds, buffer),
            HDF_LOGE("%{public}s, read buffer handle error", __func__));
        bool isValidBuffer = (ret == HDF_SUCCESS ? true : false);

        uint32_t seqNo = -1;
        bool result = true;
        DISPLAY_CHK_CONDITION(result, ret == HDF_SUCCESS, unpacker->ReadUint32(seqNo),
            HDF_LOGE("%{public}s, read seqNo error", __func__));
        ret = result ? HDF_SUCCESS : HDF_FAILURE;

        int32_t fence = -1;
        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::FileDescriptorUnpack(unpacker, inFds, fence),
            HDF_LOGE("%{public}s, FileDescriptorUnpack error", __func__));
        HdifdParcelable fdParcel(fence);
        {
            DISPLAY_CHECK(cacheMgr_ == nullptr, goto EXIT);
            std::lock_guard<std::mutex> lock(cacheMgr_->GetCacheMgrMutex());
            DeviceCache* devCache = cacheMgr_->DeviceCacheInstance(devId);
            DISPLAY_CHECK(devCache == nullptr, goto EXIT);

            ret = devCache->SetDisplayClientBuffer(buffer, seqNo, [&](const BufferHandle& handle)->int32_t {
                int rc = impl_->SetDisplayClientBuffer(devId, handle, fdParcel.GetFd());
                DISPLAY_CHK_RETURN(rc != HDF_SUCCESS, HDF_FAILURE, HDF_LOGE(" fail"));
                return HDF_SUCCESS;
            });
        }
#ifndef DISPLAY_COMMUNITY
        fdParcel.Move();
#endif // DISPLAY_COMMUNITY
EXIT:
        if (ret != HDF_SUCCESS) {
            HDF_LOGE("%{public}s, SetDisplayClientBuffer error", __func__);
            if (isValidBuffer != HDF_SUCCESS) {
                FreeBufferHandle(buffer);
            }
            errMaps_.emplace(REQUEST_CMD_SET_DISPLAY_CLIENT_BUFFER, ret);
        }

        return;
    }

    void OnSetDisplayClientDamage(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t vectSize = 0;
        bool retBool = true;
        DISPLAY_CHK_CONDITION(retBool, true, unpacker->ReadUint32(devId),
            HDF_LOGE("%{public}s, read devId error", __func__));

        DISPLAY_CHK_CONDITION(retBool, true, unpacker->ReadUint32(vectSize),
            HDF_LOGE("%{public}s, read vectSize error", __func__));

        int32_t ret = (retBool ? HDF_SUCCESS : HDF_FAILURE);
        std::vector<IRect> rects(vectSize);
        if (ret == HDF_SUCCESS) {
            for (uint32_t i = 0; i < vectSize; i++) {
                DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::RectUnpack(unpacker, rects[i]),
                    HDF_LOGE("%{public}s, read vect error at i = %{public}d", __func__, i));
                if (ret != HDF_SUCCESS) {
                    break;
                }
            }
        }
        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, impl_->SetDisplayClientDamage(devId, rects),
            HDF_LOGE("%{public}s, SetDisplayClientDamage error", __func__));

        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_DISPLAY_CLIENT_DAMAGE, ret);
        }
        return;
    }

    void OnCommit(std::shared_ptr<CommandDataUnpacker> unpacker, std::vector<HdifdInfo>& outFds)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        int32_t fence = -1;

#define DEBUG_COMPOSER_CACHE
#ifdef DEBUG_COMPOSER_CACHE
        cacheMgr_->Dump();
#endif // DEBUG_COMPOSER_CACHE
        bool retBool = true;
        DISPLAY_CHK_CONDITION(retBool, true, unpacker->ReadUint32(devId),
            HDF_LOGE("%{public}s, read devId error", __func__));

        int32_t ret = (retBool ? HDF_SUCCESS : HDF_FAILURE);
        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, impl_->Commit(devId, fence),
            HDF_LOGE("%{public}s, commit error", __func__));

        HdifdParcelable fdParcel(fence);
        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::StartSection(REPLY_CMD_COMMIT, replyPacker_),
            HDF_LOGE("%{public}s, StartSection error", __func__));

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::FileDescriptorPack(fdParcel.GetFd(), replyPacker_, outFds),
            HDF_LOGE("%{public}s, FileDescriptorPack error", __func__));

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::EndSection(replyPacker_),
            HDF_LOGE("%{public}s, EndSection error", __func__));

        replyCommandCnt_++;

#ifndef DISPLAY_COMMUNITY
        fdParcel.Move();
#endif // DISPLAY_COMMUNITY

        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_COMMIT, ret);
        }

        return;
    }

    void OnSetLayerAlpha(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        LayerAlpha alpha = {0};
        bool retBool = true;

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        retBool = unpacker->ReadBool(alpha.enGlobalAlpha);
        DISPLAY_CHECK(retBool == false, goto EXIT);

        retBool = unpacker->ReadBool(alpha.enPixelAlpha);
        DISPLAY_CHECK(retBool == false, goto EXIT);

        retBool = unpacker->ReadUint8(alpha.alpha0);
        DISPLAY_CHECK(retBool == false, goto EXIT);

        retBool = unpacker->ReadUint8(alpha.alpha1);
        DISPLAY_CHECK(retBool == false, goto EXIT);

        retBool = unpacker->ReadUint8(alpha.gAlpha);
        DISPLAY_CHECK(retBool == false, goto EXIT);

        ret = impl_->SetLayerAlpha(devId, layerId, alpha);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

EXIT:
        if (ret != HDF_SUCCESS || retBool == false) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_ALPHA, ret);
        }
        return;
    }

    void OnSetLayerRegion(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        IRect rect = {0};

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = CmdUtils::RectUnpack(unpacker, rect);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerRegion(devId, layerId, rect);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_REGION, ret);
        }
        return;
    }

    void OnSetLayerCrop(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        IRect rect = {0};

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = CmdUtils::RectUnpack(unpacker, rect);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerCrop(devId, layerId, rect);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_CROP, ret);
        }
        return;
    }

    void OnSetLayerZorder(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        uint32_t zorder = 0;

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = unpacker->ReadUint32(zorder) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerZorder(devId, layerId, zorder);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_ZORDER, ret);
        }
        return;
    }

    void OnSetLayerPreMulti(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        bool preMulti = false;

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = unpacker->ReadBool(preMulti) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerPreMulti(devId, layerId, preMulti);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_PREMULTI, ret);
        }
        return;
    }

    void OnSetLayerTransformMode(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        int32_t type = 0;

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = unpacker->ReadInt32(type) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerTransformMode(devId, layerId, static_cast<TransformType>(type));
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_TRANSFORM_MODE, ret);
        }
        return;
    }

    void OnSetLayerDirtyRegion(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        uint32_t vectSize = 0;
        int32_t ret = HDF_SUCCESS;

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId),
            HDF_LOGE("%{public}s, read devId error", __func__));

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, unpacker->ReadUint32(vectSize) ? HDF_SUCCESS : HDF_FAILURE,
            HDF_LOGE("%{public}s, read vectSize error", __func__));

        std::vector<IRect> rects(vectSize);
        if (ret == HDF_SUCCESS) {
            for (uint32_t i = 0; i < vectSize; i++) {
                DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::RectUnpack(unpacker, rects[i]),
                    HDF_LOGE("%{public}s, read vect error, at i = %{public}d", __func__, i));
                if (ret != HDF_SUCCESS) {
                    break;
                }
            }
        }

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, impl_->SetLayerDirtyRegion(devId, layerId, rects),
            HDF_LOGE("%{public}s, SetLayerDirtyRegion error", __func__));

        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_DIRTY_REGION, ret);
        }
        return;
    }

    void OnSetLayerVisibleRegion(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        uint32_t vectSize = 0;
        int32_t ret = HDF_SUCCESS;

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId),
            HDF_LOGE("%{public}s, read devId error", __func__));

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, unpacker->ReadUint32(vectSize) ? HDF_SUCCESS : HDF_FAILURE,
            HDF_LOGE("%{public}s, read vectSize error", __func__));

        std::vector<IRect> rects(vectSize);
        if (ret == HDF_SUCCESS) {
            for (uint32_t i = 0; i < vectSize; i++) {
                DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::RectUnpack(unpacker, rects[i]),
                    HDF_LOGE("%{public}s, read vect error, at i = %{public}d", __func__, i));
                if (ret != HDF_SUCCESS) {
                    break;
                }
            }
        }

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, impl_->SetLayerVisibleRegion(devId, layerId, rects),
            HDF_LOGE("%{public}s, SetLayerDirtyRegion error", __func__));

        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_VISIBLE_REGION, ret);
        }
        return;
    }

    void OnSetLayerBuffer(std::shared_ptr<CommandDataUnpacker> unpacker, const std::vector<HdifdInfo>& inFds)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        BufferHandle *buffer = nullptr;
        int32_t ret = HDF_SUCCESS;

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId),
            HDF_LOGE("%{public}s, read devId error", __func__));

        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::BufferHandleUnpack(unpacker, inFds, buffer),
            HDF_LOGE("%{public}s, read BufferHandleUnpack error", __func__));
        bool isValidBuffer = (ret == HDF_SUCCESS ? true : false);

        int32_t seqNo = -1;
        bool result = true;
        DISPLAY_CHK_CONDITION(result, ret == HDF_SUCCESS, unpacker->ReadInt32(seqNo),
            HDF_LOGE("%{public}s, read seqNo error", __func__));
        ret = result ? HDF_SUCCESS : HDF_FAILURE;

        int32_t fence = -1;
        DISPLAY_CHK_CONDITION(ret, HDF_SUCCESS, CmdUtils::FileDescriptorUnpack(unpacker, inFds, fence),
            HDF_LOGE("%{public}s, FileDescriptorUnpack error", __func__));

        HdifdParcelable fdParcel(fence);

        // unpack deletingList
        uint32_t vectSize = 0;
        DISPLAY_CHK_CONDITION(result, ret == HDF_SUCCESS, unpacker->ReadUint32(vectSize),
            HDF_LOGE("%{public}s, read vectSize error", __func__));

        std::vector<uint32_t> deletingList(vectSize);
        for (int32_t i = 0; i < vectSize; i++) {
            DISPLAY_CHK_CONDITION(result, true, unpacker->ReadUint32(deletingList[i]),
                HDF_LOGE("%{public}s, read seqNo error, at i = %{public}d", __func__, i));
            if (result != true) {
                break;
            }
        }
        ret = result ? HDF_SUCCESS : HDF_FAILURE;
        {
            DISPLAY_CHECK(cacheMgr_ == nullptr, goto EXIT);
            std::lock_guard<std::mutex> lock(cacheMgr_->GetCacheMgrMutex());
            DeviceCache* devCache = nullptr;
            LayerCache* layerCache = nullptr;
            devCache = cacheMgr_->DeviceCacheInstance(devId);
            DISPLAY_CHECK(devCache == nullptr, goto EXIT);
            layerCache = devCache->LayerCacheInstance(layerId);
            DISPLAY_CHECK(layerCache == nullptr, goto EXIT);

            ret = layerCache->SetLayerBuffer(buffer, seqNo, deletingList, [&](const BufferHandle& handle)->int32_t {
                DumpLayerBuffer(devId, layerId, fence, handle);

                int rc = impl_->SetLayerBuffer(devId, layerId, handle, fdParcel.GetFd());
                DISPLAY_CHK_RETURN(rc != HDF_SUCCESS, HDF_FAILURE, HDF_LOGE(" fail"));
                return HDF_SUCCESS;
            });
        }
#ifndef DISPLAY_COMMUNITY
        fdParcel.Move();
#endif // DISPLAY_COMMUNITY
EXIT:
        if (ret != HDF_SUCCESS) {
            HDF_LOGE("%{public}s, SetLayerBuffer error", __func__);
            if (isValidBuffer != HDF_SUCCESS) {
                FreeBufferHandle(buffer);
            }
            errMaps_.emplace(REQUEST_CMD_SET_DISPLAY_CLIENT_BUFFER, ret);
        }

        return;
    }

    void OnSetLayerCompositionType(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        int32_t type;
        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = unpacker->ReadInt32(type) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerCompositionType(devId, layerId, static_cast<CompositionType>(type));
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_COMPOSITION_TYPE, ret);
        }
        return;
    }

    void OnSetLayerBlendType(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        int32_t type;
        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = unpacker->ReadInt32(type) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerBlendType(devId, layerId, static_cast<BlendType>(type));
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_BLEND_TYPE, ret);
        }
        return;
    }

    void OnSetLayerMaskInfo(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        uint32_t maskInfo = 0;

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = unpacker->ReadUint32(maskInfo) ? HDF_SUCCESS : HDF_FAILURE;
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerMaskInfo(devId, layerId, static_cast<MaskInfo>(maskInfo));
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_MASK_INFO, ret);
        }
        return;
    }

    void OnSetLayerColor(std::shared_ptr<CommandDataUnpacker> unpacker)
    {
        DISPLAY_TRACE;

        uint32_t devId = 0;
        uint32_t layerId = 0;
        LayerColor layerColor = {0};

        int32_t ret = CmdUtils::SetupDeviceUnpack(unpacker, devId, layerId);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = CmdUtils::LayerColorUnpack(unpacker, layerColor);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);

        ret = impl_->SetLayerColor(devId, layerId, layerColor);
        DISPLAY_CHECK(ret != HDF_SUCCESS, goto EXIT);
EXIT:
        if (ret != HDF_SUCCESS) {
            errMaps_.emplace(REQUEST_CMD_SET_LAYER_COLOR, ret);
        }
        return;
    }

    int32_t PeriodDataReset()
    {
        replyCommandCnt_ = 0;
        errMaps_.clear();

        int32_t ret = CmdUtils::StartPack(CONTROL_CMD_REPLY_BEGIN, replyPacker_);
        if (ret != HDF_SUCCESS) {
            HDF_LOGE("PackBegin failure, ret=%{public}d", ret);
        }
        return ret;
    }

    static std::string GetFileName(uint32_t devId, uint32_t layerId, const BufferHandle& buffer)
    {
        struct timeval tv;
        char nowStr[TIME_BUFFER_MAX_LEN] = {0};

        gettimeofday(&tv, nullptr);
        if (strftime(nowStr, sizeof(nowStr), "%m-%d-%H-%M-%S", localtime(&tv.tv_sec)) == 0) {
            HDF_LOGE("strftime failed");
            return "";
        };

        std::ostringstream strStream;
        strStream << "hdi_layer_" << devId << "_" << layerId << "_" << buffer.width << "x" << buffer.height << "_" <<
            nowStr << "-" << tv.tv_usec;
        return strStream.str();
    }

    static void DumpLayerBuffer(uint32_t devId, uint32_t layerId, int32_t fence, const BufferHandle& buffer)
    {
        const std::string SWITCH_ON = "on";
        const uint32_t DUMP_BUFFER_SWITCH_LEN = 4;
        char dumpSwitch[DUMP_BUFFER_SWITCH_LEN] = {0};
        GetParameter("hdi.composer.dumpbuffer", "off", dumpSwitch, DUMP_BUFFER_SWITCH_LEN);

        if (SWITCH_ON.compare(dumpSwitch) != 0) {
            return;
        }

        const uint32_t FENCE_TIMEOUT = 3000;
        int32_t retCode = WaitFence(fence, FENCE_TIMEOUT);
        if (retCode != HDF_SUCCESS) {
            return;
        }

        if (g_bufferServiceImpl == nullptr) {
            g_bufferServiceImpl = IMapper::Get(true);
            DISPLAY_CHECK((g_bufferServiceImpl == nullptr), HDF_LOGE("get IMapper failed"));
        }

        std::string fileName = GetFileName(devId, layerId, buffer);
        DISPLAY_CHECK((fileName == ""), HDF_LOGE("GetFileName failed"));
        HDF_LOGI("fileName = %{public}s", fileName.c_str());

        const std::string PATH_PREFIX = "/data/local/traces/";
        std::stringstream filePath;
        filePath << PATH_PREFIX << fileName;
        std::ofstream rawDataFile(filePath.str(), std::ofstream::binary);
        DISPLAY_CHECK((!rawDataFile.good()), HDF_LOGE("open file failed, %{public}s",
            std::strerror(errno)));

        sptr<NativeBuffer> hdiBuffer = new NativeBuffer();
        hdiBuffer->SetBufferHandle(const_cast<BufferHandle*>(&buffer));

        int32_t ret = 0;
        ret = g_bufferServiceImpl->Mmap(hdiBuffer);
        DISPLAY_CHECK((ret != HDF_SUCCESS), HDF_LOGE("Mmap buffer failed"));

        std::chrono::milliseconds time_before = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()
        );
        rawDataFile.write(static_cast<const char *>(buffer.virAddr), buffer.size);
        std::chrono::milliseconds time_after = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()
        );
        HDF_LOGI("wirte file take time %{public}lld", time_after.count() - time_before.count());
        rawDataFile.close();

        ret = g_bufferServiceImpl->Unmap(hdiBuffer);
        DISPLAY_CHECK((ret != HDF_SUCCESS), HDF_LOGE("Unmap buffer failed"));
    }

    static int32_t WaitFence(int32_t fence, uint32_t timeout)
    {
        int retCode = -1;
        if (fence < 0) {
            HDF_LOGE("The fence id is invalid.");
            return retCode;
        }

        struct pollfd pollfds = {0};
        pollfds.fd = fence;
        pollfds.events = POLLIN;

        do {
            retCode = poll(&pollfds, 1, timeout);
        } while (retCode == -1 && (errno == EINTR || errno == EAGAIN));

        if (retCode == 0) {
            retCode = -1;
            errno = ETIME;
        } else if (retCode > 0) {
            retCode = 0;
            if (pollfds.revents & (POLLERR | POLLNVAL)) {
                retCode = -1;
                errno = EINVAL;
            }
        }

        return retCode < 0 ? -errno : HDF_SUCCESS;
    }

private:
    VdiImpl* impl_ = nullptr;
    std::shared_ptr<DeviceCacheManager> cacheMgr_;
    std::shared_ptr<Transfer> request_;
    bool isReplyUpdated_;
    std::shared_ptr<Transfer> reply_;
    /* period data */
    uint32_t replyCommandCnt_;
    std::shared_ptr<CommandDataPacker> replyPacker_;
    std::unordered_map<int32_t, int32_t> errMaps_;
};
using HdiDisplayCmdResponser = DisplayCmdResponser<SharedMemQueue<int32_t>, IDisplayComposerVdi>;
} // namespace V1_0
} // namespace Composer
} // namespace Display
} // namespace HDI
} // namespace OHOS
#endif // OHOS_HDI_DISPLAY_V1_0_DISPLAY_CMD_REQUESTER_H
