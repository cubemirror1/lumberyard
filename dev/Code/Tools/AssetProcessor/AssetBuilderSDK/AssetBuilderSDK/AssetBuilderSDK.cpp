
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
#include "AssetBuilderSDK.h"
#include "AssetBuilderBusses.h"

//////////////////////////////////////////////////////////////////////////
//currently only needed for GETFAKEASSETYPE
#include <AzFramework/StringFunc/StringFunc.h>
#include <AzCore/std/string/wildcard.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/XML/rapidxml.h>
#include <AzCore/XML/rapidxml_print.h>
#include <AzCore/Component/Entity.h> // so we can have the entity UUID type.
#include <AzFramework/IO/LocalFileIO.h>
#include <AzCore/Component/ComponentApplicationBus.h>
//////////////////////////////////////////////////////////////////////////

namespace AssetBuilderSDK
{
    const char* const ErrorWindow = "Error"; //Use this window name to log error messages.
    const char* const WarningWindow = "Warning"; //Use this window name to log warning messages.
    const char* const InfoWindow = "Info"; //Use this window name to log info messages.

    const char* const s_processJobRequestFileName = "ProcessJobRequest.xml";
    const char* const s_processJobResponseFileName = "ProcessJobResponse.xml";

    // for now, we're going to put our various masks that are widely known in here.
    // we may expand this into a 64-bit "namespace" by adding additional 32 bits at the front at some point, if it becomes necessary.
    const AZ::u32 SUBID_MASK_ID         = 0x0000FFFF;
    const AZ::u32 SUBID_MASK_LOD_LEVEL  = 0x000F0000;
    const AZ::u32 SUBID_LOD_LEVEL_SHIFT = 16; // shift 16 bits to the left to get 0x000F0000
    const AZ::u32 SUBID_FLAG_DIFF       = 0x00100000;
    const AZ::u32 SUBID_FLAG_ALPHA      = 0x00200000;

    AZ::u32 GetSubID_ID(AZ::u32 packedSubId)
    {
        return packedSubId & SUBID_MASK_ID;
    }

    AZ::u32 GetSubID_LOD(AZ::u32 packedSubId)
    {
        return ((packedSubId & SUBID_MASK_LOD_LEVEL) >> SUBID_LOD_LEVEL_SHIFT);
    }

    AZ::u32 ConstructSubID(AZ::u32 subIndex, AZ::u32 lodLevel, AZ::u32 fromSubIndex)
    {
        AZ_Warning(WarningWindow, subIndex <= SUBID_MASK_ID, "ConstructSubID: subIndex %u is too big to fit", subIndex);
        AZ_Warning(WarningWindow, lodLevel <= 0xF, "ConstructSubID: lodLevel %u is too big to fit", lodLevel);
        AZ::u32 mask = ~(SUBID_MASK_ID | SUBID_MASK_LOD_LEVEL);
        AZ::u32 original = fromSubIndex & mask;  // eliminate all the bits that are part of the subid or the lod index

        fromSubIndex = original;
        fromSubIndex |= subIndex;
        fromSubIndex |= (lodLevel << SUBID_LOD_LEVEL_SHIFT) & SUBID_MASK_LOD_LEVEL;

        AZ_Warning(WarningWindow, original == (fromSubIndex & mask), "ConstructSubID: Unexpected modification of the bits that should not have been touched");

        return fromSubIndex;
    }

#if defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
    // this function exists merely to retain code compatibility with older versions.
    // it is recommended to upgrade to the new way, which is to just use the m_enabledPlatforms structs.
    Platform LEGACY_ConvertNewPlatformIdentifierToOldPlatform(const char* newPlatformName)
    {
        if (azstricmp(newPlatformName, "pc") == 0)
        {
            return AssetBuilderSDK::Platform_PC;
        }
        if (azstricmp(newPlatformName, "es3") == 0)
        {
            return AssetBuilderSDK::Platform_ES3;
        }
        if (azstricmp(newPlatformName, "ios") == 0)
        {
            return AssetBuilderSDK::Platform_IOS;
        }
        if (azstricmp(newPlatformName, "osx_gl") == 0)
        {
            return AssetBuilderSDK::Platform_OSX;
        }
        if (azstricmp(newPlatformName, "xboxone") == 0)
        {
            return AssetBuilderSDK::Platform_XBOXONE;  // ACCEPTED_USE
        }
        if (azstricmp(newPlatformName, "ps4") == 0)
        {
            return AssetBuilderSDK::Platform_PS4;  // ACCEPTED_USE
        }

        return AssetBuilderSDK::Platform_NONE;
    }

    // this function exists merely to retain code compatibility with older versions.
    // it is recommended to upgrade to the new way, which is to just use the m_enabledPlatforms structs.
    const char* LEGACY_ConvertOldPlatformToNewPlatformIdentifier(Platform oldPlatform)
    {
        switch (oldPlatform)
        {
        case AssetBuilderSDK::Platform_PC:
            return "pc";
        case AssetBuilderSDK::Platform_ES3:
            return "es3";
        case AssetBuilderSDK::Platform_IOS:
            return "ios";
        case AssetBuilderSDK::Platform_OSX:
            return "osx_gl";
        case AssetBuilderSDK::Platform_XBOXONE:     // ACCEPTED_USE
            return "xboxone";
        case AssetBuilderSDK::Platform_PS4:     // ACCEPTED_USE
            return "ps4";
        }
        return "unknown platform";
    }

#endif // defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)

    void BuilderLog(AZ::Uuid builderId, const char* message, ...)
    {
        va_list args;
        va_start(args, message);
        EBUS_EVENT(AssetBuilderSDK::AssetBuilderBus, BuilderLog, builderId, message, args);
        va_end(args);
    }

    AssetBuilderPattern::AssetBuilderPattern(const AZStd::string& pattern, PatternType type)
        : m_pattern(pattern)
        , m_type(type)
    {
    }

    AZStd::string AssetBuilderPattern::ToString() const
    {
        return AZStd::string::format("{%s:%s}", m_type == Wildcard ? "WildCard" : "Regex", m_pattern.c_str());
    }

    void AssetBuilderPattern::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<AssetBuilderPattern>()
                ->Version(1)
                ->Field("Pattern", &AssetBuilderPattern::m_pattern)
                ->Field("Type", &AssetBuilderPattern::m_type);
        }
    }

    FilePatternMatcher::FilePatternMatcher(const AssetBuilderSDK::AssetBuilderPattern& pattern)
        : m_pattern(pattern)
    {
        if (pattern.m_type == AssetBuilderSDK::AssetBuilderPattern::Regex)
        {
            m_isRegex = true;
            m_isValid = FilePatternMatcher::ValidatePatternRegex(pattern.m_pattern, m_errorString);
            if (m_isValid)
            {
                this->m_regex = RegexType(pattern.m_pattern.c_str(), RegexType::flag_type::icase | RegexType::flag_type::ECMAScript);
            }
        }
        else
        {
            m_isValid = true;
            m_isRegex = false;
        }
    }

    FilePatternMatcher::FilePatternMatcher(const AZStd::string& pattern, AssetBuilderSDK::AssetBuilderPattern::PatternType type)
        : FilePatternMatcher(AssetBuilderSDK::AssetBuilderPattern(pattern, type))
    {
    }

    FilePatternMatcher::FilePatternMatcher(const FilePatternMatcher& copy)
        : m_pattern(copy.m_pattern)
        , m_regex(copy.m_regex)
        , m_isRegex(copy.m_isRegex)
        , m_isValid(copy.m_isValid)
        , m_errorString(copy.m_errorString)
    {
    }

    FilePatternMatcher& FilePatternMatcher::operator=(const FilePatternMatcher& copy)
    {
        this->m_pattern = copy.m_pattern;
        this->m_regex = copy.m_regex;
        this->m_isRegex = copy.m_isRegex;
        this->m_isValid = copy.m_isValid;
        this->m_errorString = copy.m_errorString;
        return *this;
    }

    bool FilePatternMatcher::MatchesPath(const AZStd::string& assetPath) const
    {
        bool matches = false;
        if (this->m_isRegex)
        {
            matches = AZStd::regex_match(assetPath.c_str(), this->m_regex);
        }
        else
        {
            matches = AZStd::wildcard_match(this->m_pattern.m_pattern, assetPath);
        }
        return matches;
    }

    bool FilePatternMatcher::IsValid() const
    {
        return this->m_isValid;
    }

    AZStd::string FilePatternMatcher::GetErrorString() const
    {
        return m_errorString;
    }

    const AssetBuilderSDK::AssetBuilderPattern& FilePatternMatcher::GetBuilderPattern() const
    {
        return this->m_pattern;
    }

    bool FilePatternMatcher::ValidatePatternRegex(const AZStd::string& pattern, AZStd::string& errorString)
    {
        AssertAbsorber absorber;
        AZStd::regex validate_regex(pattern.c_str(),
            AZStd::regex::flag_type::icase |
            AZStd::regex::flag_type::ECMAScript);

        return absorber.m_assertMessage.empty();
    }

    bool AssetBuilderDesc::IsExternalBuilder() const
    {
        return m_builderType == AssetBuilderType::External;
    }

    /**
    * New constructor - uses the platform Identifier from the PlatformInfo passed into Create Jobs.
    */
    JobDescriptor::JobDescriptor(const AZStd::string& additionalFingerprintInfo, AZStd::string jobKey, const char* platformIdentifier)
        : m_additionalFingerprintInfo(additionalFingerprintInfo)
        , m_jobKey(jobKey)
    {
        SetPlatformIdentifier(platformIdentifier);
    }

#if defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
    /**
    * old api constructor.  Still supported for backward compatibility, but do not use in new code.
    */
    JobDescriptor::JobDescriptor(AZStd::string additionalFingerprintInfo, int platform, const AZStd::string& jobKey)
        : m_additionalFingerprintInfo(additionalFingerprintInfo)
        , m_platform(platform)
        , m_jobKey(jobKey)
    {
        SetPlatformIdentifier(LEGACY_ConvertOldPlatformToNewPlatformIdentifier(static_cast<AssetBuilderSDK::Platform>(m_platform)));
    }
#endif // defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)

    void JobDescriptor::SetPlatformIdentifier(const char* platformIdentifier)
    {
        if (platformIdentifier)
        {
            m_platformIdentifier = platformIdentifier;
        }
#if defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
        m_platform = LEGACY_ConvertNewPlatformIdentifierToOldPlatform(platformIdentifier);
#endif // defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
    }

    const AZStd::string& JobDescriptor::GetPlatformIdentifier() const
    {
        return m_platformIdentifier;
    }

#if defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
    namespace Internal
    {
        // for legacy compatibility, we make sure that if only the m_platform field is populated
        // we go ahead and fill out the new API from the old one.
        class JobDescriptorSerializeEventHandler
            : public AZ::SerializeContext::IEventHandler
        {
        public:
            virtual void OnReadBegin(void* classPtr)
            {
                JobDescriptor* populatingJobDescriptor = reinterpret_cast<JobDescriptor*>(classPtr);
                if (populatingJobDescriptor)
                {
                    // before we serialize this instance into a stream, lets make sure its converted.
                    if (populatingJobDescriptor->GetPlatformIdentifier().empty())
                    {
                        populatingJobDescriptor->SetPlatformIdentifier(LEGACY_ConvertOldPlatformToNewPlatformIdentifier(static_cast<AssetBuilderSDK::Platform>(populatingJobDescriptor->m_platform)));
                    }
                }
            }

            virtual void OnWriteEnd(void* classPtr)
            {
                JobDescriptor* populatingJobDescriptor = reinterpret_cast<JobDescriptor*>(classPtr);
                if (populatingJobDescriptor)
                {
                    // we've finished writing into this instance, lets patch up the platform.
                    if (populatingJobDescriptor->GetPlatformIdentifier().empty())
                    {
                        populatingJobDescriptor->SetPlatformIdentifier(LEGACY_ConvertOldPlatformToNewPlatformIdentifier(static_cast<AssetBuilderSDK::Platform>(populatingJobDescriptor->m_platform)));
                    }
                }
            }
        };

        static JobDescriptorSerializeEventHandler s_jobDescriptorSerializeEventHandlerInstance;
    }

#endif // defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)


    void JobDescriptor::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<JobDescriptor>()->
                Version(4)->
                Field("Additional Fingerprint Info", &JobDescriptor::m_additionalFingerprintInfo)->
#if defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
            EventHandler(&Internal::s_jobDescriptorSerializeEventHandlerInstance)->
                Field("Platform", &JobDescriptor::m_platform)->  // note:  deprecated but we still pass it via the network so it must be serialized.
#endif // defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
            Field("Platform Identifier", &JobDescriptor::m_platformIdentifier)->     // new API
                Field("Job Key", &JobDescriptor::m_jobKey)->
                Field("Critical", &JobDescriptor::m_critical)->
                Field("Priority", &JobDescriptor::m_priority)->
                Field("Job Parameters", &JobDescriptor::m_jobParameters)->
                Field("Check Exclusive Lock", &JobDescriptor::m_checkExclusiveLock)->
                Field("Fail On Error", &JobDescriptor::m_failOnError)->
                Field("Job Dependency List", &JobDescriptor::m_jobDependencyList)->
                Field("Check Server", &JobDescriptor::m_checkServer);
        }
    }

    CreateJobsRequest::CreateJobsRequest(AZ::Uuid builderid, AZStd::string sourceFile, AZStd::string watchFolder, const AZStd::vector<PlatformInfo>& enabledPlatforms, const AZ::Uuid& sourceFileUUID)
        : m_builderid(builderid)
        , m_sourceFile(sourceFile)
        , m_watchFolder(watchFolder)
        , m_enabledPlatforms(enabledPlatforms)
        , m_sourceFileUUID(sourceFileUUID)
    {
        // synthesize m_platformFlags from the rest
    }

    CreateJobsRequest::CreateJobsRequest()
    {
    }

    bool CreateJobsRequest::HasPlatform(const char* platformIdentifier) const
    {
        if (!platformIdentifier)
        {
            AZ_Assert(false, "HasPlatform called with nullptr platformIdentifier.");
            return false;
        }
        for (const PlatformInfo& info : m_enabledPlatforms)
        {
            if (azstricmp(info.m_identifier.c_str(), platformIdentifier) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool CreateJobsRequest::HasPlatformWithTag(const char* platformTag) const
    {
        if (!platformTag)
        {
            AZ_Assert(false, "HasPlatformWithTag called with nullptr platformTag.");
            return false;
        }

        for (const PlatformInfo& info : m_enabledPlatforms)
        {
            if (info.HasTag(platformTag))
            {
                return true;
            }
        }
        return false;
    }

#if defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)
    size_t CreateJobsRequest::GetEnabledPlatformsCount() const
    {
        return m_enabledPlatforms.size();
    }

    AssetBuilderSDK::Platform CreateJobsRequest::GetEnabledPlatformAt(size_t index) const
    {
        AZ_WarningOnce(AssetBuilderSDK::WarningWindow, false, "This builder is calling a deprecated function: GetEnabledPlatformAt.  Consider just using the new m_enabledPlatforms member instead.");
        if (index >= m_enabledPlatforms.size())
        {
            // for old compat, we cannot assert here.
            return AssetBuilderSDK::Platform_NONE;
        }
        const PlatformInfo& info = m_enabledPlatforms[index];

        return LEGACY_ConvertNewPlatformIdentifierToOldPlatform(info.m_identifier.c_str());
    }

    bool CreateJobsRequest::IsPlatformEnabled(AZ::u32 platform) const
    {
        AZ_WarningOnce(AssetBuilderSDK::WarningWindow, false, "This builder is calling a deprecated function: IsPlatformEnabled.  Consider just using the new m_enabledPlatforms member instead.");
        for (const PlatformInfo& info : m_enabledPlatforms)
        {
            if (static_cast<AZ::u32>(LEGACY_ConvertNewPlatformIdentifierToOldPlatform(info.m_identifier.c_str())) == platform)
            {
                return true;
            }
        }
        return false;
    }

    bool CreateJobsRequest::IsPlatformValid(AZ::u32 platform) const
    {
        AZ_WarningOnce(AssetBuilderSDK::WarningWindow, false, "This builder is calling a deprecated function: IsPlatformValid.  Consider just using the new m_enabledPlatforms member instead.");
        return ((platform & AllPlatforms) == platform);
    }
#endif // defined(ENABLE_LEGACY_PLATFORMFLAGS_SUPPORT)

    PlatformInfo::PlatformInfo(const char* identifier, const AZStd::unordered_set<AZStd::string>& tags)
        : m_identifier(identifier)
        , m_tags(tags)
    {
    }

    bool PlatformInfo::HasTag(const char* tag) const
    {
        return m_tags.find(tag) != m_tags.end();
    }

    bool PlatformInfo::operator==(const PlatformInfo& other)
    {
        return (this->m_identifier == other.m_identifier);
    }

    void PlatformInfo::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<PlatformInfo>()->
                Version(1)->
                Field("Platform Identifier", &PlatformInfo::m_identifier)->
                Field("Tags on Platform", &PlatformInfo::m_tags);
        }
    }

    AZStd::string PlatformInfo::PlatformVectorAsString(const AZStd::vector<PlatformInfo>& platforms)
    {
        AZStd::string platformString;
        for (const auto& platformInfo : platforms)
        {
            if (!platformString.empty())
            {
                platformString.append(", ");
            }
            platformString.append(platformInfo.m_identifier.c_str());
        }

        return platformString;
    }

    void CreateJobsRequest::Reflect(AZ::ReflectContext* context)
    {
        PlatformInfo::Reflect(context);
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<CreateJobsRequest>()->
                Version(2)->
                Field("Builder Id", &CreateJobsRequest::m_builderid)->
                Field("Watch Folder", &CreateJobsRequest::m_watchFolder)->
                Field("Source File", &CreateJobsRequest::m_sourceFile)->
                Field("Enabled Platforms", &CreateJobsRequest::m_enabledPlatforms)->
                Field("Source File UUID", &CreateJobsRequest::m_sourceFileUUID);
        }
    }

    ProductDependency::ProductDependency(AZ::Data::AssetId dependencyId, const AZStd::bitset<64>& flags)
        : m_dependencyId(dependencyId)
        , m_flags(flags)
    {
    }

    void ProductDependency::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<ProductDependency>()->
                Version(1)->
                Field("Dependency Id", &ProductDependency::m_dependencyId)->
                Field("Flags", &ProductDependency::m_flags);
        }
    }

    ProductPathDependency::ProductPathDependency(AZStd::string_view dependencyPath, ProductPathDependencyType dependencyType)
        : m_dependencyPath(dependencyPath),
        m_dependencyType(dependencyType)
    {
    }

    bool ProductPathDependency::operator==(const ProductPathDependency& rhs) const
    {
        return m_dependencyPath == rhs.m_dependencyPath
            && m_dependencyType == rhs.m_dependencyType;
    }

    void ProductPathDependency::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<ProductPathDependency>()->
                Version(1)->
                Field("Dependency Path", &ProductPathDependency::m_dependencyPath)->
                Field("Dependency Type", &ProductPathDependency::m_dependencyType);
        }
    }

    JobProduct::JobProduct(const AZStd::string& productName, AZ::Data::AssetType productAssetType /*= AZ::Data::AssetType::CreateNull() */, AZ::u32 productSubID /*= 0*/)
        : m_productFileName(productName)
        , m_productAssetType(productAssetType)
        , m_productSubID(productSubID)
    {
        //////////////////////////////////////////////////////////////////////////
        //REMOVE THIS
        //when builders output asset type, guess by file extension
        if (m_productAssetType.IsNull())
        {
            m_productAssetType = InferAssetTypeByProductFileName(m_productFileName.c_str());
        }
        if (m_productSubID == 0)
        {
            m_productSubID = InferSubIDFromProductFileName(m_productAssetType, m_productFileName.c_str());
        }
        //////////////////////////////////////////////////////////////////////////
    }

    JobProduct::JobProduct(AZStd::string&& productName, AZ::Data::AssetType productAssetType /*= AZ::Data::AssetType::CreateNull() */, AZ::u32 productSubID /*= 0*/)
        : m_productFileName(AZStd::move(productName))
        , m_productAssetType(productAssetType)
        , m_productSubID(productSubID)
    {
        //////////////////////////////////////////////////////////////////////////
        //REMOVE THIS
        //when builders output asset type, guess by file extension
        if (m_productAssetType.IsNull())
        {
            m_productAssetType = InferAssetTypeByProductFileName(m_productFileName.c_str());
        }
        if (m_productSubID == 0)
        {
            m_productSubID = InferSubIDFromProductFileName(m_productAssetType, m_productFileName.c_str());
        }
        //////////////////////////////////////////////////////////////////////////
    }

    //////////////////////////////////////////////////////////////////////////
    // the following block is for legacy compatibility
    // all new assets should either place their desired UUIDs in the productAssetType field in the actual assetProcessorPlatformConfig.ini file
    // or should create an actual Builder-SDK builder which can specify the id and typeid very specifically.

    // the following three extensions can have splitted LOD files
    static const char* textureExtensions = ".dds";
    static const char* staticMeshExtensions = ".cgf";
    static const char* skinnedMeshExtensions = ".skin";
    static const char* materialExtensions = ".mtl";
    // splitted lods have the following extensions:
    static const char* mipsAndLodsExtensions = ".1 .2 .3 .4 .5 .6 .7 .8 .9 .a .1a .2a .3a .4a .5a .6a .7a .8a .9a";
    // XML files may contain generic data (avoid this in new builders - use a custom extension!)
    static const char* xmlExtensions = ".xml";
    static const char* geomCacheExtensions = ".cax";
    static const char* skeletonExtensions = ".chr";

    static AZ::Data::AssetType unknownAssetType = AZ::Data::AssetType::CreateNull();

    // as real BuilderSDK builders are created for these types, they will no longer need to be matched by extension
    // and can be emitted by the builder itself, which has knowledge of the type.
    // first, we'll do the ones which are randomly assigned because they did not actually have an asset type or handler in the main engine yet
    static AZ::Data::AssetType textureMipsAssetType("{3918728C-D3CA-4D9E-813E-A5ED20C6821E}");
    static AZ::Data::AssetType skinnedMeshLodsAssetType("{58E5824F-C27B-46FD-AD48-865BA41B7A51}");
    static AZ::Data::AssetType staticMeshLodsAssetType("{9AAE4926-CB6A-4C60-9948-A1A22F51DB23}");
    static AZ::Data::AssetType geomCacheAssetType("{EBC96071-E960-41B6-B3E3-328F515AE5DA}");
    static AZ::Data::AssetType skeletonAssetType("{60161B46-21F0-4396-A4F0-F2CCF0664CDE}");

    // now the ones that are actual asset types that already have an AssetData-derived class in the engine
    // note that ideally, all NEW asset types beyond this point are instead built by an actual specific builder-SDK derived builder
    // and thus can emit their own asset types, but for legacy compatibility, this is an alternate means to do this.
    static AZ::Data::AssetType textureAssetType("{59D5E20B-34DB-4D8E-B867-D33CC2556355}"); // from MaterialAsset.h
    static AZ::Data::AssetType materialAssetType("{F46985B5-F7FF-4FCB-8E8C-DC240D701841}"); // from MaterialAsset.h
    static AZ::Data::AssetType meshAssetType("{C2869E3B-DDA0-4E01-8FE3-6770D788866B}"); // from MeshAsset.h
    static AZ::Data::AssetType skinnedMeshAssetType("{C5D443E1-41FF-4263-8654-9438BC888CB7}"); // from MeshAsset.h
    static AZ::Data::AssetType sliceAssetType("{C62C7A87-9C09-4148-A985-12F2C99C0A45}"); // from SliceAsset.h
    static AZ::Data::AssetType dynamicSliceAssetType("{78802ABF-9595-463A-8D2B-D022F906F9B1}"); // from SliceAsset.h

    // the following Asset Types are discovered in generic XMLs.  in the future, these need to be custom file extensions
    // and this data can move from here to the INI file, or into a custom builder.
    static AZ::Data::AssetType mannequinControllerDefinitionAssetType("{49375937-7F37-41B1-96A5-B099A8657DDE}"); // from MannequinAsset.h
    static AZ::Data::AssetType prefabsLibraryAssetType("{2DC3C556-9461-4729-8313-2BA0CB64EF52}"); // from PrefabsLibraryAssetTypeInfo.cpp
    static AZ::Data::AssetType entityPrototypeLibraryAssetType("{B034F8AB-D881-4A35-A408-184E3FDEB2FE}"); // from EntityPrototypeLibraryAssetTypeInfo.cpp
    static AZ::Data::AssetType gameTokenAssetType("{1D4B56F8-366A-4040-B645-AE87E3A00DAB}"); // from GameTokenAssetTypeInfo.cpp
    static AZ::Data::AssetType particleLibraryAssetType("{99542BB9-2870-4DD0-AA0A-57B5541CD196}");
    static AZ::Data::AssetType mannequinPreviewType("{1FFF61EE-044A-4A72-87D0-60870FF02C58}"); // from MannequinControllerAssetTypeInfo.cpp
    static AZ::Data::AssetType particleAssetType("{6EB56B55-1B58-4EE3-A268-27680338AE56}"); // from ParticleAsset.h
    static AZ::Data::AssetType lensFlareAssetType("{CF44D1F0-F178-4A3D-A9E6-D44721F50C20}"); // from LensFlareAsset.h
    static AZ::Data::AssetType fontAssetType("{57767D37-0EBE-43BE-8F60-AB36D2056EF8}"); // form UiAssetTypes.h
    static AZ::Data::AssetType uiCanvasAssetType("{E48DDAC8-1F1E-4183-AAAB-37424BCC254B}"); // from UiAssetTypes.h
    static AZ::Data::AssetType behaviorTreeAssetType("{0DB1F34B-EB30-4318-A20B-CF035F419E74}"); // from BehaviorTreeAsset.h

    // EMotionFX Gem types
    // If we have a way to register gem specific asset type in the future, we can remove this.
    static const char* emotionFXActorExtension = ".actor";
    static const char* emotionFXMotionExtension = ".motion";
    static const char* emotionFXMotionSetExtension = ".motionset";
    static const char* emotionFXAnimGraphExtension = ".animgraph";
    static AZ::Data::AssetType emotionFXActorAssetType("{F67CC648-EA51-464C-9F5D-4A9CE41A7F86}"); // from ActorAsset.h in EMotionFX Gem
    static AZ::Data::AssetType emotionFXMotionAssetType("{00494B8E-7578-4BA2-8B28-272E90680787}"); // from MotionAsset.h in EMotionFX Gem
    static AZ::Data::AssetType emotionFXMotionSetAssetType("{1DA936A0-F766-4B2F-B89C-9F4C8E1310F9}"); // from MotionSetAsset.h in EMotionFX Gem
    static AZ::Data::AssetType emotionFXAnimGraphAssetType("{28003359-4A29-41AE-8198-0AEFE9FF5263}"); // from AnimGraphAsset.h in EMotionFX Gem

    AZ::Data::AssetType JobProduct::InferAssetTypeByProductFileName(const char* productFile)
    {
        //get the extension
        AZStd::string extension;
        if (!AzFramework::StringFunc::Path::GetExtension(productFile, extension, true))
        {
            // files which have no extension at all are not currently supported
            return unknownAssetType;
        }

        // intercept texture mips and mesh lods first
        size_t pos = AZStd::string::npos;
        pos = AzFramework::StringFunc::Find(mipsAndLodsExtensions, extension.c_str());
        if (pos != AZStd::string::npos)
        {
            // Could be a texture mip or a model lod...
            // we don't want them to have the same asset type as the main asset,
            // otherwise they would be assignable in the editor for that type.

            // is it a texture mip?
            AZStd::vector<AZStd::string> textureExtensionsList;
            AzFramework::StringFunc::Tokenize(textureExtensions, textureExtensionsList);
            for (const auto& textureExtension : textureExtensionsList)
            {
                pos = AzFramework::StringFunc::Find(productFile, textureExtension.c_str());
                if (pos != AZStd::string::npos)
                {
                    return textureMipsAssetType;
                }
            }

            // if its not a texture mip is it a static mesh lod?
            AZStd::vector<AZStd::string> staticMeshExtensionsList;
            AzFramework::StringFunc::Tokenize(staticMeshExtensions, staticMeshExtensionsList);
            for (const auto& staticMeshExtension : staticMeshExtensionsList)
            {
                pos = AzFramework::StringFunc::Find(productFile, staticMeshExtension.c_str());
                if (pos != AZStd::string::npos)
                {
                    return staticMeshLodsAssetType;
                }
            }

            // if its not a static mesh lod is it a skinned mesh lod?
            AZStd::vector<AZStd::string> skinnedMeshExtensionsList;
            AzFramework::StringFunc::Tokenize(skinnedMeshExtensions, skinnedMeshExtensionsList);
            for (const auto& skinnedMeshExtension : skinnedMeshExtensionsList)
            {
                pos = AzFramework::StringFunc::Find(productFile, skinnedMeshExtension.c_str());
                if (pos != AZStd::string::npos)
                {
                    return skinnedMeshLodsAssetType;
                }
            }
        }

        if (AzFramework::StringFunc::Find(textureExtensions, extension.c_str()) != AZStd::string::npos)
        {
            return textureAssetType;
        }

        if (AzFramework::StringFunc::Find(materialExtensions, extension.c_str()) != AZStd::string::npos)
        {
            return materialAssetType;
        }

        if (AzFramework::StringFunc::Find(staticMeshExtensions, extension.c_str()) != AZStd::string::npos)
        {
            return meshAssetType;
        }

        if (AzFramework::StringFunc::Find(skinnedMeshExtensions, extension.c_str()) != AZStd::string::npos)
        {
            return skinnedMeshAssetType;
        }

        if (AzFramework::StringFunc::Find(geomCacheExtensions, extension.c_str()) != AZStd::string::npos)
        {
            return geomCacheAssetType;
        }

        if (AzFramework::StringFunc::Find(skeletonExtensions, extension.c_str()) != AZStd::string::npos)
        {
            return skeletonAssetType;
        }

        // EMFX Gem Begin
        // If we have a way to register gem specific asset type in the future, we can remove this.
        if (AzFramework::StringFunc::Find(emotionFXActorExtension, extension.c_str()) != AZStd::string::npos)
        {
            return emotionFXActorAssetType;
        }

        if (AzFramework::StringFunc::Find(emotionFXMotionExtension, extension.c_str()) != AZStd::string::npos)
        {
            return emotionFXMotionAssetType;
        }

        if (AzFramework::StringFunc::Find(emotionFXMotionSetExtension, extension.c_str()) != AZStd::string::npos)
        {
            return emotionFXMotionSetAssetType;
        }

        if (AzFramework::StringFunc::Find(emotionFXAnimGraphExtension, extension.c_str()) != AZStd::string::npos)
        {
            return emotionFXAnimGraphAssetType;
        }
        // EMFX Gem End

        // if its an XML file then we may need to open it up to find out what it is...
        if (AzFramework::StringFunc::Find(xmlExtensions, extension.c_str()) != AZStd::string::npos)
        {
            if (!AZ::IO::SystemFile::Exists(productFile))
            {
                return unknownAssetType;
            }

            uint64_t fileSize = AZ::IO::SystemFile::Length(productFile);
            if (fileSize == 0)
            {
                return unknownAssetType;
            }

            AZStd::vector<char> buffer(fileSize + 1);
            buffer[fileSize] = 0;
            if (!AZ::IO::SystemFile::Read(productFile, buffer.data()))
            {
                return unknownAssetType;
            }

            // if it contains this kind of element, we save that info for later once we confirm its an objectstream.
            bool contains_UIAssetCanvasElement = (AzFramework::StringFunc::Find(buffer.data(), "{50B8CF6C-B19A-4D86-AFE9-96EFB820D422}") != AZStd::string::npos);

            // this is why new asset types REALLY need to have an extension (or other indicator) on their source or product that are different and can easily determine their
            // intended usage.
            AZ::rapidxml::xml_document<char>* xmlDoc = azcreate(AZ::rapidxml::xml_document<char>, (), AZ::SystemAllocator, "BuilderSDK Temp XML Reader");
            if (xmlDoc->parse<AZ::rapidxml::parse_no_data_nodes>(buffer.data()))
            {
                // note that PARSE_FASTEST does not null-terminate strings, instead we just PARSE_NO_DATA_NODES so that xdata and other such blobs are ignored since they don't matter
                AZ::rapidxml::xml_node<char>* xmlRootNode = xmlDoc->first_node();
                if (!xmlRootNode)
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return unknownAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "fontshader"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return fontAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "ParticleLibrary"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return particleAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "BehaviorTree"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return behaviorTreeAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "LensFlareLibrary"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return lensFlareAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "ControllerDef"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return mannequinControllerDefinitionAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "PrefabsLibrary"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return prefabsLibraryAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "EntityPrototypeLibrary"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return entityPrototypeLibraryAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "GameTokensLibrary"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return gameTokenAssetType;
                }

                if (!azstricmp(xmlRootNode->name(), "MannequinPreview"))
                {
                    azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                    return mannequinPreviewType;
                }

                if (!azstricmp(xmlRootNode->name(), "ObjectStream")) // this is an object stream, which means the actual class in the object stream is the first child.
                {
                    if (contains_UIAssetCanvasElement)
                    {
                        azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                        return uiCanvasAssetType;
                    }

                    for (AZ::rapidxml::xml_node<char>* childNode = xmlRootNode->first_node(); childNode; childNode = childNode->next_sibling())
                    {
                        // the old object-stream format used to put the name of the type as the actual <element> so we have to just check it for a 'type' flag.
                        AZ::rapidxml::xml_attribute<char>* attr = childNode->first_attribute("type", 0, false);
                        if (attr)
                        {
                            // note that this will issue a warning if its a malformed UUID.
                            AZ::Data::AssetType attrType(attr->value());

                            if (attrType != AZ::Data::AssetType::CreateNull())
                            {
                                azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
                                return attrType;
                            }
                        }
                    }
                }
            }
            azdestroy(xmlDoc, AZ::SystemAllocator, AZ::rapidxml::xml_document<char>);
        }
        return unknownAssetType;
    }

    //////////////////////////////////////////////////////////////////////////
    // This is for e
    AZ::u32 JobProduct::InferSubIDFromProductFileName(const AZ::Data::AssetType& assetType, const char* productFile)
    {
        // The engine only uses dynamic slice files, but for right now slices are also copy products...
        // So slice will have two products, so they must have a different sub id's.
        // In the interest of future compatibility we will want dynamic slices to have a 0 sub id, so set the slice copy product
        // sub id's to 1. The only reason they are currently copy products is for the builder to
        // make dynamic slice products. This will change in the future and the .slice files will no longer copy themselves
        // as products, so this is a temporary rule and eventually there will only be 0's
        if (assetType == sliceAssetType)
        {
            return 1;
        }

        // Dynamic slices should use subId = 0 to avoid ambiguity with legacy editor slice guids.
        if (assetType == dynamicSliceAssetType)
        {
            return 2;
        }

        //get the extension
        AZStd::string extension;
        if (!AzFramework::StringFunc::Path::GetExtension(productFile, extension, true))
        {
            return 0; //no extension....the safest thing is 0 and see if we get any collisions
        }

        //intercept mips and lods first
        bool isTextureMip = assetType == textureMipsAssetType;
        bool isStaticMeshLod = assetType == staticMeshLodsAssetType;
        bool isSkinnedMeshLod = assetType == skinnedMeshLodsAssetType;
        bool isTexture = assetType == textureAssetType;

        //if its a static or skinned mesh, then its not a lod so return 0
        if ((assetType == skinnedMeshAssetType) || (assetType == meshAssetType))
        {
            return 0;
        }

        //////////////////////////////////////////////////////////////////////////
        //calculated sub ids
        AZ::u32 subID = 0;

        // if its texture or texture mip there is a special case for diff-textures
        // it is special because a single FILENAME_CM.TIF can become -many- outputs:
        // filename_cm_diff.dds
        // filename_cm_diff.dds.1
        // filename_cm_diff.dds.1a
        // ...
        // filename_cm_diff.dds.9
        // filename_cm_diff.dds.9a
        // filename_cm.dds
        // filename_cm.dds.1
        // filename_cm.dds.1a
        // ...
        // filename_cm.dds.9
        // filename_cm.dds.9a

        if (isTexture || isTextureMip)
        {
            //but it could be a special case for _diff. textures
            if (AzFramework::StringFunc::Find(productFile, "_diff.") != AZStd::string::npos)
            {
                subID |= SUBID_FLAG_DIFF; // 'diff' textures will have the 6th bit set.  This still leaves us with 0..31 as valid mips
            }
        }

        if (isTexture)
        {
            return subID; //if its texture and not a mip, so it gets 0 or 100
        }

        if (isTextureMip || isStaticMeshLod || isSkinnedMeshLod)
        {
            //if its a mip or lod add to the subid, so .1 should be 1, .2 should be 2 etc.. if its a diff mips its will be 101, 102, etc...
            if ((extension[extension.size() - 1]) == 'a')
            {
                // if it ends with a .a, its the next set
                subID = subID | SUBID_FLAG_ALPHA;
                extension.resize(extension.size() - 1);
            }

            for (int idx = 1; idx <= 9; ++idx)
            {
                AZStd::string check = AZStd::string::format(".%i", idx);
                if (check == extension)
                {
                    subID = ConstructSubID(0, idx, subID);
                    break;
                }
            }

            // note that if its JUST '.a' then it will end up here with 0 added.

            return subID;
        }

        return 0; //zero by default
    }

    void JobProduct::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<JobProduct>()->
                Version(5)->
                Field("Product File Name", &JobProduct::m_productFileName)->
                Field("Product Asset Type", &JobProduct::m_productAssetType)->
                Field("Product Sub Id", &JobProduct::m_productSubID)->
                Field("Legacy Sub Ids", &JobProduct::m_legacySubIDs)->
                Field("Dependencies", &JobProduct::m_dependencies)->
                Field("Relative Path Dependencies", &JobProduct::m_pathDependencies);
        }
    }

    //////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////

    void ProcessJobRequest::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<ProcessJobRequest>()->
                Version(2)->
                Field("Source File", &ProcessJobRequest::m_sourceFile)->
                Field("Watch Folder", &ProcessJobRequest::m_watchFolder)->
                Field("Full Path", &ProcessJobRequest::m_fullPath)->
                Field("Builder Guid", &ProcessJobRequest::m_builderGuid)->
                Field("Job Description", &ProcessJobRequest::m_jobDescription)->
                Field("Temp Dir Path", &ProcessJobRequest::m_tempDirPath)->
                Field("Platform Info", &ProcessJobRequest::m_platformInfo)->
                Field("Source File Dependency List", &ProcessJobRequest::m_sourceFileDependencyList)->
                Field("Source File UUID", &ProcessJobRequest::m_sourceFileUUID);
        }
    }

    void ProcessJobResponse::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<ProcessJobResponse>()->
                Version(2)->
                Field("Output Products", &ProcessJobResponse::m_outputProducts)->
                Field("Result Code", &ProcessJobResponse::m_resultCode)->
                Field("Requires SubId Generation", &ProcessJobResponse::m_requiresSubIdGeneration);
        }
    }

    bool ProcessJobResponse::Succeeded() const
    {
        return m_resultCode == ProcessJobResultCode::ProcessJobResult_Success;
    }

    void InitializeSerializationContext()
    {
        AZ::SerializeContext* serializeContext = nullptr;

        EBUS_EVENT_RESULT(serializeContext, AZ::ComponentApplicationBus, GetSerializeContext);
        AZ_Assert(serializeContext, "Unable to retrieve serialize context.");

        ProductPathDependency::Reflect(serializeContext);
        SourceFileDependency::Reflect(serializeContext);
        JobDependency::Reflect(serializeContext);
        JobDescriptor::Reflect(serializeContext);
        AssetBuilderPattern::Reflect(serializeContext);
        ProductDependency::Reflect(serializeContext);
        JobProduct::Reflect(serializeContext);
        AssetBuilderDesc::Reflect(serializeContext);

        RegisterBuilderRequest::Reflect(serializeContext);
        RegisterBuilderResponse::Reflect(serializeContext);
        CreateJobsRequest::Reflect(serializeContext);
        CreateJobsResponse::Reflect(serializeContext);
        ProcessJobRequest::Reflect(serializeContext);
        ProcessJobResponse::Reflect(serializeContext);

        BuilderHelloRequest::Reflect(serializeContext);
        BuilderHelloResponse::Reflect(serializeContext);
        CreateJobsNetRequest::Reflect(serializeContext);
        CreateJobsNetResponse::Reflect(serializeContext);
        ProcessJobNetRequest::Reflect(serializeContext);
        ProcessJobNetResponse::Reflect(serializeContext);
    }

    AssetBuilderSDK::JobCancelListener::JobCancelListener(AZ::u64 jobId)
        : m_cancelled(false)
    {
        JobCommandBus::Handler::BusConnect(jobId);
    }

    AssetBuilderSDK::JobCancelListener::~JobCancelListener()
    {
        JobCommandBus::Handler::BusDisconnect();
    }

    void AssetBuilderSDK::JobCancelListener::Cancel()
    {
        m_cancelled = true;
    }

    bool AssetBuilderSDK::JobCancelListener::IsCancelled() const
    {
        return m_cancelled;
    }

    AZStd::string SourceFileDependency::ToString() const
    {
        return AZStd::string::format("SourceFileDependency UUID: %s NAME: %s", m_sourceFileDependencyUUID.ToString<AZStd::string>().c_str(), m_sourceFileDependencyPath.c_str());
    }

    void SourceFileDependency::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<SourceFileDependency>()->
                Version(2)->
                Field("Source File Dependency Path", &SourceFileDependency::m_sourceFileDependencyPath)->
                Field("Source File Dependency UUID", &SourceFileDependency::m_sourceFileDependencyUUID)->
                Field("Source Dependency Type", &SourceFileDependency::m_sourceDependencyType);
        }
    }

    void RegisterBuilderRequest::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<RegisterBuilderRequest>()->
                Version(1)->
                Field("FilePath", &RegisterBuilderRequest::m_filePath);
        }
    }

    void AssetBuilderDesc::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<AssetBuilderDesc>()
                ->Version(2)
                ->Field("Flags", &AssetBuilderDesc::m_flags)
                ->Field("Name", &AssetBuilderDesc::m_name)
                ->Field("Patterns", &AssetBuilderDesc::m_patterns)
                ->Field("BusId", &AssetBuilderDesc::m_busId)
                ->Field("Version", &AssetBuilderDesc::m_version)
                ->Field("AnalysisFingerprint", &AssetBuilderDesc::m_analysisFingerprint);
        }
    }

    void RegisterBuilderResponse::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<RegisterBuilderResponse>()
                ->Version(1)
                ->Field("Asset Builder Desc List", &RegisterBuilderResponse::m_assetBuilderDescList);
        }
    }

    bool CreateJobsResponse::Succeeded() const
    {
        return m_result == CreateJobsResultCode::Success;
    }

    void CreateJobsResponse::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<CreateJobsResponse>()->
                Version(1)->
                Field("Result Code", &CreateJobsResponse::m_result)->
                Field("Source File Dependency List", &CreateJobsResponse::m_sourceFileDependencyList)->
                Field("Create Job Outputs", &CreateJobsResponse::m_createJobOutputs);
        }
    }

    void BuilderHelloRequest::Reflect(AZ::ReflectContext* context)
    {
        auto serialize = azrtti_cast<AZ::SerializeContext*>(context);
        if (serialize)
        {
            serialize->Class<BuilderHelloRequest>()
                ->Version(1)
                ->Field("UUID", &BuilderHelloRequest::m_uuid);
        }
    }

    unsigned int BuilderHelloRequest::MessageType()
    {
        static unsigned int messageType = AZ_CRC("AssetBuilderSDK::BuilderHelloRequest", 0x213a7248);

        return messageType;
    }

    unsigned int BuilderHelloRequest::GetMessageType() const
    {
        return MessageType();
    }

    void BuilderHelloResponse::Reflect(AZ::ReflectContext* context)
    {
        auto serialize = azrtti_cast<AZ::SerializeContext*>(context);
        if (serialize)
        {
            serialize->Class<BuilderHelloResponse>()
                ->Version(1)
                ->Field("Accepted", &BuilderHelloResponse::m_accepted)
                ->Field("UUID", &BuilderHelloResponse::m_uuid);
        }
    }

    unsigned int BuilderHelloResponse::GetMessageType() const
    {
        return BuilderHelloRequest::MessageType();
    }

    //////////////////////////////////////////////////////////////////////////

    void CreateJobsNetRequest::Reflect(AZ::ReflectContext* context)
    {
        auto serialize = azrtti_cast<AZ::SerializeContext*>(context);
        if (serialize)
        {
            serialize->Class<CreateJobsNetRequest>()
                ->Version(1)
                ->Field("Request", &CreateJobsNetRequest::m_request);
        }
    }

    unsigned int CreateJobsNetRequest::MessageType()
    {
        static unsigned int messageType = AZ_CRC("AssetBuilderSDK::CreateJobsNetRequest", 0xc48209c0);

        return messageType;
    }

    unsigned int CreateJobsNetRequest::GetMessageType() const
    {
        return MessageType();
    }

    void CreateJobsNetResponse::Reflect(AZ::ReflectContext* context)
    {
        auto serialize = azrtti_cast<AZ::SerializeContext*>(context);
        if (serialize)
        {
            serialize->Class<CreateJobsNetResponse>()
                ->Version(1)
                ->Field("Response", &CreateJobsNetResponse::m_response);
        }
    }

    unsigned int CreateJobsNetResponse::GetMessageType() const
    {
        return CreateJobsNetRequest::MessageType();
    }



    void ProcessJobNetRequest::Reflect(AZ::ReflectContext* context)
    {
        auto serialize = azrtti_cast<AZ::SerializeContext*>(context);
        if (serialize)
        {
            serialize->Class<ProcessJobNetRequest>()
                ->Version(1)
                ->Field("Request", &ProcessJobNetRequest::m_request);
        }
    }

    unsigned int ProcessJobNetRequest::MessageType()
    {
        static unsigned int messageType = AZ_CRC("AssetBuilderSDK::ProcessJobNetRequest", 0x479f340f);

        return messageType;
    }

    unsigned int ProcessJobNetRequest::GetMessageType() const
    {
        return MessageType();
    }

    void ProcessJobNetResponse::Reflect(AZ::ReflectContext* context)
    {
        auto serialize = azrtti_cast<AZ::SerializeContext*>(context);
        if (serialize)
        {
            serialize->Class<ProcessJobNetResponse>()
                ->Version(1)
                ->Field("Response", &ProcessJobNetResponse::m_response);
        }
    }

    unsigned int ProcessJobNetResponse::GetMessageType() const
    {
        return ProcessJobNetRequest::MessageType();
    }

    JobDependency::JobDependency(const AZStd::string& jobKey, const AZStd::string& platformIdentifier, const JobDependencyType& type, const SourceFileDependency& sourceFile)
        : m_jobKey(jobKey)
        , m_platformIdentifier(platformIdentifier)
        , m_type(type)
        , m_sourceFile(sourceFile)
    {
    }

    void JobDependency::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<JobDependency>()->
                Version(1)->
                Field("Source File", &JobDependency::m_sourceFile)->
                Field("Job Key", &JobDependency::m_jobKey)->
                Field("Platform Identifier", &JobDependency::m_platformIdentifier)->
                Field("Job Dependency Type", &JobDependency::m_type);
        }
    }

    AssertAbsorber::AssertAbsorber()
    {
        // only absorb asserts when this object is on scope in the thread that this object is on scope in.
        s_onAbsorbThread = true;
        BusConnect();
    }

    AssertAbsorber::~AssertAbsorber()
    {
        s_onAbsorbThread = false;
        BusDisconnect();
    }

    bool AssertAbsorber::OnAssert(const char* message)
    {
        if (s_onAbsorbThread)
        {
            m_assertMessage = message;
            return true; // I handled this, do not forward it
        }
        return false;
    }

    AZ_THREAD_LOCAL bool AssertAbsorber::s_onAbsorbThread = false;
}
