/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#pragma once

#define AZ_TRAIT_CRYSOUNDSYSTEM_ATL_POOL_SIZE 4 << 10 /* 4 MiB (re-evaluate this size!) */
#define AZ_TRAIT_CRYSOUNDSYSTEM_ATL_POOL_SIZE_DEFAULT_TEXT "4096 (4 MiB)"
#define AZ_TRAIT_CRYSOUNDSYSTEM_AUDIO_EVENT_POOL_SIZE 128
#define AZ_TRAIT_CRYSOUNDSYSTEM_AUDIO_EVENT_POOL_SIZE_DEFAULT_TEXT "128"
#define AZ_TRAIT_CRYSOUNDSYSTEM_AUDIO_OBJECT_POOL_SIZE 256
#define AZ_TRAIT_CRYSOUNDSYSTEM_AUDIO_OBJECT_POOL_SIZE_DEFAULT_TEXT "256"
#define AZ_TRAIT_CRYSOUNDSYSTEM_AUDIO_THREAD_AFFINITY AFFINITY_MASK_ALL
#define AZ_TRAIT_CRYSOUNDSYSTEM_CAN_INCLUDE_AUDIO_PRODUCTION_CODE 1
#define AZ_TRAIT_CRYSOUNDSYSTEM_DEFAULT_AUDIO_SYSTEM_IMPLEMENTATION_NAME "CryAudioImplWwise"
#define AZ_TRAIT_CRYSOUNDSYSTEM_FILE_CACHE_MANAGER_ALLOCATION_POLICY IMemoryManager::eapCustomAlignment
#define AZ_TRAIT_CRYSOUNDSYSTEM_FILE_CACHE_MANAGER_SIZE 72 << 10 /* 72 MiB (re-evaluate this size!) */
#define AZ_TRAIT_CRYSOUNDSYSTEM_FILE_CACHE_MANAGER_SIZE_DEFAULT_TEXT "2048 (2 MiB)"