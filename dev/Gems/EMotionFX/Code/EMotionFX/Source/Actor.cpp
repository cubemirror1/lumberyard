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

#include "EMotionFXConfig.h"
#include "Actor.h"
#include "Motion.h"
#include "SkeletalMotion.h"
#include "MeshDeformerStack.h"
#include "Material.h"
#include "Mesh.h"
#include "SubMesh.h"
#include "MorphMeshDeformer.h"
#include "SkinningInfoVertexAttributeLayer.h"
#include "ActorInstance.h"
#include "MotionSystem.h"
#include "EventManager.h"
#include "EventHandler.h"
#include "NodeMap.h"
#include "NodeGroup.h"
#include "ActorManager.h"
#include "MorphSetup.h"
#include "Node.h"
#include "Skeleton.h"
#include "SoftSkinDeformer.h"
#include "DualQuatSkinDeformer.h"
#include "DebugDraw.h"
#include <EMotionFX/Source/SimulatedObjectSetup.h>

#include <MCore/Source/IDGenerator.h>
#include <MCore/Source/Compare.h>
#include <MCore/Source/Quaternion.h>
#include <MCore/Source/OBB.h>

namespace EMotionFX
{
    AZ_CLASS_ALLOCATOR_IMPL(Actor, ActorAllocator, 0)

    Actor::NodeInfo::NodeInfo()
    {
        mOBB.Init();
    }


    Actor::LODLevel::LODLevel()
    {
    }


    Actor::NodeLODInfo::NodeLODInfo()
    {
        mMesh   = nullptr;
        mStack  = nullptr;
    }


    Actor::NodeLODInfo::~NodeLODInfo()
    {
        MCore::Destroy(mMesh);
        MCore::Destroy(mStack);
    }

    //----------------------------------------------------

    // constructor
    Actor::Actor(const char* name)
        : BaseObject()
    {
        SetName(name);

        // setup the array memory categories
        mMaterials.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        mDependencies.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        mMorphSetups.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        mLODs.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);

        mSkeleton = Skeleton::Create();

        // init some members
        mLODs.AddEmpty();
        mLODs[0].mNodeInfos.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);

        mMotionExtractionNode       = MCORE_INVALIDINDEX32;
        mRetargetRootNode           = MCORE_INVALIDINDEX32;
        mThreadIndex                = 0;
        mCustomData                 = nullptr;
        mID                         = MCore::GetIDGenerator().GenerateID();
        mUnitType                   = GetEMotionFX().GetUnitType();
        mFileUnitType               = mUnitType;

        mUsedForVisualization       = false;
        mDirtyFlag                  = false;

        m_physicsSetup              = AZStd::make_shared<PhysicsSetup>();
        m_simulatedObjectSetup      = AZStd::make_shared<SimulatedObjectSetup>();

#if defined(EMFX_DEVELOPMENT_BUILD)
        mIsOwnedByRuntime           = false;
#endif // EMFX_DEVELOPMENT_BUILD

        // make sure we have at least allocated the first LOD of materials and facial setups
        mMaterials.Reserve(4);  // reserve space for 4 lods
        mMorphSetups.Reserve(4); //
        mMaterials.AddEmpty();
        mMaterials[0].SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        mMorphSetups.Add(nullptr);

        GetActorManager().RegisterActor(this);
        GetEventManager().OnCreateActor(this);
    }


    // destructor
    Actor::~Actor()
    {
        // trigger the OnDeleteActor event
        GetEventManager().OnDeleteActor(this);

        // clear the node mirror data
        mNodeMirrorInfos.Clear(true);

        // delete all the materials
        RemoveAllMaterials();

        // remove all morph setups
        RemoveAllMorphSetups();

        // remove all node groups
        RemoveAllNodeGroups();

        mInvBindPoseTransforms.clear();

        // destroy the skeleton
        MCore::Destroy(mSkeleton);

        // unregister the actor
        GetActorManager().UnregisterActor(this);
    }


    // create method
    Actor* Actor::Create(const char* name)
    {
        return aznew Actor(name);
    }


    // creates a clone of the actor (a copy).
    // does NOT copy the motions and motion tree
    Actor* Actor::Clone()
    {
        // create the new actor and set the name and filename
        Actor* result = Actor::Create(GetName());
        result->SetFileName(GetFileName());

        // copy the actor attributes
        result->mMotionExtractionNode   = mMotionExtractionNode;
        result->mUnitType               = mUnitType;
        result->mFileUnitType           = mFileUnitType;
        result->mStaticAABB             = mStaticAABB;
        result->mRetargetRootNode       = mRetargetRootNode;
        result->mInvBindPoseTransforms  = mInvBindPoseTransforms;

        result->RecursiveAddDependencies(this);

        // clone all nodes groups
        for (uint32 i = 0; i < mNodeGroups.GetLength(); ++i)
        {
            result->AddNodeGroup(aznew NodeGroup(*mNodeGroups[i]));
        }

        // clone the materials
        result->mMaterials.Resize(mMaterials.GetLength());
        for (uint32 i = 0; i < mMaterials.GetLength(); ++i)
        {
            // get the number of materials in the current LOD
            const uint32 numMaterials = mMaterials[i].GetLength();
            result->mMaterials[i].Reserve(numMaterials);
            for (uint32 m = 0; m < numMaterials; ++m)
            {
                // retrieve the current material
                Material* material = mMaterials[i][m];

                // clone the material
                Material* clone = material->Clone();

                // add the cloned material to the cloned actor
                result->AddMaterial(i, clone);
            }
        }

        // clone the skeleton
        MCore::Destroy(result->mSkeleton);
        result->mSkeleton = mSkeleton->Clone();

        // clone lod data
        result->SetNumLODLevels(mLODs.GetLength());
        result->mNodeInfos = mNodeInfos;
        for (uint32 i = 0; i < mLODs.GetLength(); ++i)
        {
            const uint32 numNodes = mSkeleton->GetNumNodes();
            result->mLODs[i].mNodeInfos.Resize(numNodes);
            for (uint32 n = 0; n < numNodes; ++n)
            {
                NodeLODInfo& resultNodeInfo = result->mLODs[i].mNodeInfos[n];
                const NodeLODInfo& sourceNodeInfo = mLODs[i].mNodeInfos[n];
                resultNodeInfo.mMesh        = (sourceNodeInfo.mMesh) ? sourceNodeInfo.mMesh->Clone()     : nullptr;
                resultNodeInfo.mStack       = (sourceNodeInfo.mStack) ? sourceNodeInfo.mStack->Clone(resultNodeInfo.mMesh)        : nullptr;
            }
        }

        // clone the morph setups
        result->mMorphSetups.Resize(mMorphSetups.GetLength());
        for (uint32 i = 0; i < mMorphSetups.GetLength(); ++i)
        {
            if (mMorphSetups[i])
            {
                result->SetMorphSetup(i, mMorphSetups[i]->Clone());
            }
            else
            {
                result->SetMorphSetup(i, nullptr);
            }
        }

        // make sure the number of root nodes is still the same
        MCORE_ASSERT(result->GetSkeleton()->GetNumRootNodes() == mSkeleton->GetNumRootNodes());

        // copy the transform data
        result->CopyTransformsFrom(this);

        result->mNodeMirrorInfos = mNodeMirrorInfos;
        result->m_physicsSetup = m_physicsSetup;
        result->SetSimulatedObjectSetup(m_simulatedObjectSetup->Clone(result));

        GetEMotionFX().GetEventManager()->OnPostCreateActor(result);

        return result;
    }

    void Actor::SetSimulatedObjectSetup(const AZStd::shared_ptr<SimulatedObjectSetup>& setup)
    {
        m_simulatedObjectSetup = setup;
    }

    // init node mirror info
    void Actor::AllocateNodeMirrorInfos()
    {
        const uint32 numNodes = mSkeleton->GetNumNodes();
        mNodeMirrorInfos.Resize(numNodes);

        // init the data
        for (uint32 i = 0; i < numNodes; ++i)
        {
            mNodeMirrorInfos[i].mSourceNode = static_cast<uint16>(i);
            mNodeMirrorInfos[i].mAxis       = MCORE_INVALIDINDEX8;
            mNodeMirrorInfos[i].mFlags      = 0;
        }
    }

    // remove the node mirror info
    void Actor::RemoveNodeMirrorInfos()
    {
        mNodeMirrorInfos.Clear(true);
    }


    // check if we have our axes detected
    bool Actor::GetHasMirrorAxesDetected() const
    {
        if (mNodeMirrorInfos.GetLength() == 0)
        {
            return false;
        }

        for (uint32 i = 0; i < mNodeMirrorInfos.GetLength(); ++i)
        {
            if (mNodeMirrorInfos[i].mAxis == MCORE_INVALIDINDEX8)
            {
                return false;
            }
        }

        return true;
    }


    // removes all materials from the actor
    void Actor::RemoveAllMaterials()
    {
        // for all LODs
        for (uint32 i = 0; i < mMaterials.GetLength(); ++i)
        {
            // delete all materials
            const uint32 numMats = mMaterials[i].GetLength();
            for (uint32 m = 0; m < numMats; ++m)
            {
                mMaterials[i][m]->Destroy();
            }
        }

        mMaterials.Clear();
    }


    // add a LOD level and copy the data from the last LOD level to the new one
    void Actor::AddLODLevel(bool copyFromLastLODLevel)
    {
        mLODs.AddEmpty();
        LODLevel& newLOD = mLODs.GetLast();
        const uint32 numNodes = mSkeleton->GetNumNodes();
        newLOD.mNodeInfos.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        newLOD.mNodeInfos.Resize(numNodes);

        const uint32 numLODs    = mLODs.GetLength();
        const uint32 lodIndex   = numLODs - 1;

        // get the number of nodes, iterate through them, create a new LOD level and copy over the meshes from the last LOD level
        for (uint32 i = 0; i < numNodes; ++i)
        {
            NodeLODInfo& newLODInfo = mLODs[lodIndex].mNodeInfos[i];
            if (copyFromLastLODLevel && lodIndex > 0)
            {
                const NodeLODInfo& prevLODInfo = mLODs[lodIndex - 1].mNodeInfos[i];
                newLODInfo.mMesh        = (prevLODInfo.mMesh)        ? prevLODInfo.mMesh->Clone()                    : nullptr;
                newLODInfo.mStack       = (prevLODInfo.mStack)       ? prevLODInfo.mStack->Clone(newLODInfo.mMesh)   : nullptr;
            }
            else
            {
                newLODInfo.mMesh        = nullptr;
                newLODInfo.mStack       = nullptr;
            }
        }

        // create a new material array for the new LOD level
        mMaterials.Resize(mLODs.GetLength());
        mMaterials[lodIndex].SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);

        // create an empty morph setup for the new LOD level
        mMorphSetups.Add(nullptr);

        // copy data from the previous LOD level if wanted
        if (copyFromLastLODLevel && numLODs > 0)
        {
            CopyLODLevel(this, lodIndex - 1, numLODs - 1, true, false);
        }
    }


    // insert a LOD level at a given position
    void Actor::InsertLODLevel(uint32 insertAt)
    {
        mLODs.Insert(insertAt);
        LODLevel& newLOD = mLODs[insertAt];
        const uint32 lodIndex   = insertAt;
        const uint32 numNodes   = mSkeleton->GetNumNodes();
        newLOD.mNodeInfos.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        newLOD.mNodeInfos.Resize(numNodes);

        // get the number of nodes, iterate through them, create a new LOD level and copy over the meshes from the last LOD level
        for (uint32 i = 0; i < numNodes; ++i)
        {
            NodeLODInfo& lodInfo    = mLODs[lodIndex].mNodeInfos[i];
            lodInfo.mMesh           = nullptr;
            lodInfo.mStack          = nullptr;
        }

        // create a new material array for the new LOD level
        mMaterials.Insert(insertAt);
        mMaterials[lodIndex].SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);

        // create an empty morph setup for the new LOD level
        mMorphSetups.Insert(insertAt, nullptr);
    }


    // replace existing LOD level with the current actor
    void Actor::CopyLODLevel(Actor* copyActor, uint32 copyLODLevel, uint32 replaceLODLevel, bool copySkeletalLODFlags, bool delLODActorFromMem)
    {
        const LODLevel& sourceLOD = copyActor->mLODs[copyLODLevel];
        LODLevel& targetLOD = mLODs[replaceLODLevel];

        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Node* node     = mSkeleton->GetNode(i);
            Node* copyNode = copyActor->GetSkeleton()->FindNodeByID(node->GetID());

            if (copyNode == nullptr)
            {
                MCore::LogWarning("Actor::CopyLODLevel() - Failed to find node '%s' in the actor we want to copy from.", node->GetName());
            }

            const NodeLODInfo& sourceNodeInfo = sourceLOD.mNodeInfos[ copyNode->GetNodeIndex() ];
            NodeLODInfo& targetNodeInfo = targetLOD.mNodeInfos[i];

            // first get rid of existing data
            MCore::Destroy(targetNodeInfo.mMesh);
            targetNodeInfo.mMesh        = nullptr;
            MCore::Destroy(targetNodeInfo.mStack);
            targetNodeInfo.mStack       = nullptr;

            // if the node exists in both models
            if (copyNode)
            {
                // copy over the mesh and collision mesh
                if (sourceNodeInfo.mMesh)
                {
                    targetNodeInfo.mMesh = sourceNodeInfo.mMesh->Clone();
                }

                // handle the stacks
                if (sourceNodeInfo.mStack)
                {
                    targetNodeInfo.mStack = sourceNodeInfo.mStack->Clone(targetNodeInfo.mMesh);
                }

                // copy the skeletal LOD flag
                if (copySkeletalLODFlags)
                {
                    node->SetSkeletalLODStatus(replaceLODLevel, copyNode->GetSkeletalLODStatus(copyLODLevel));
                }
            }
        }

        // copy the materials
        const uint32 numMaterials = copyActor->GetNumMaterials(copyLODLevel);
        for (uint32 i = 0; i < mMaterials[replaceLODLevel].GetLength(); ++i)
        {
            mMaterials[replaceLODLevel][i]->Destroy();
        }
        mMaterials[replaceLODLevel].Clear();
        mMaterials[replaceLODLevel].Reserve(numMaterials);
        for (uint32 i = 0; i < numMaterials; ++i)
        {
            AddMaterial(replaceLODLevel, copyActor->GetMaterial(copyLODLevel, i)->Clone());
        }

        // copy the morph setup
        if (mMorphSetups[replaceLODLevel])
        {
            mMorphSetups[replaceLODLevel]->Destroy();
        }

        if (copyActor->GetMorphSetup(copyLODLevel))
        {
            mMorphSetups[replaceLODLevel] = copyActor->GetMorphSetup(copyLODLevel)->Clone();
        }
        else
        {
            mMorphSetups[replaceLODLevel] = nullptr;
        }

        // remove the actor from memory if desired
        if (delLODActorFromMem)
        {
            copyActor->Destroy();
        }
    }


    // preallocate memory for all LOD levels
    void Actor::SetNumLODLevels(uint32 numLODs)
    {
        mLODs.Resize(numLODs);
        for (uint32 i = 0; i < numLODs; ++i)
        {
            mLODs[i].mNodeInfos.SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        }

        // reserve space for the materials
        mMaterials.Resize(numLODs);
        for (uint32 i = 0; i < numLODs; ++i)
        {
            mMaterials[i].SetMemoryCategory(EMFX_MEMCATEGORY_ACTORS);
        }

        // reserve space for the morph setups
        mMorphSetups.Resize(numLODs);
        for (uint32 i = 0; i < numLODs; ++i)
        {
            mMorphSetups[i] = nullptr;
        }
    }


    // remove a given LOD level
    void Actor::RemoveLODLevel(uint32 lodLevel)
    {
        LODLevel& lodLevelToRemove = mLODs[lodLevel];

        // iterate through all nodes and remove the meshes and mesh deformers for the given LOD level
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            NodeLODInfo& nodeInfo = lodLevelToRemove.mNodeInfos[i];
            MCore::Destroy(nodeInfo.mMesh);
            nodeInfo.mMesh      = nullptr;
            MCore::Destroy(nodeInfo.mStack);
            nodeInfo.mStack     = nullptr;
        }
        mLODs.Remove(lodLevel);

        // iterate through the materials, reset them and remove the material LOD level
        const uint32 numMaterials = GetNumMaterials(lodLevel);
        for (uint32 i = 0; i < numMaterials; ++i)
        {
            mMaterials[lodLevel][i]->Destroy();
        }
        mMaterials.Remove(lodLevel);

        // remove the morph target LOD level
        if (mMorphSetups[lodLevel])
        {
            mMorphSetups[lodLevel]->Destroy();
        }
        mMorphSetups.Remove(lodLevel);
    }


    // remove all LOD levels except for the highest LOD level
    void Actor::RemoveAllLODLevels()
    {
        while (mLODs.GetLength() > 1)
        {
            RemoveLODLevel(mLODs.GetLength() - 1);
        }
    }


    // removes all node meshes and stacks
    void Actor::RemoveAllNodeMeshes()
    {
        const uint32 numNodes = mSkeleton->GetNumNodes();

        const uint32 numLODs = mLODs.GetLength();
        for (uint32 lod = 0; lod < numLODs; ++lod)
        {
            LODLevel& lodLevel = mLODs[lod];
            for (uint32 i = 0; i < numNodes; ++i)
            {
                NodeLODInfo& info = lodLevel.mNodeInfos[i];
                MCore::Destroy(info.mMesh);
                info.mMesh      = nullptr;
                MCore::Destroy(info.mStack);
                info.mStack     = nullptr;
            }
        }
    }


    void Actor::CalcMeshTotals(uint32 lodLevel, uint32* outNumPolygons, uint32* outNumVertices, uint32* outNumIndices) const
    {
        uint32 totalPolys   = 0;
        uint32 totalVerts   = 0;
        uint32 totalIndices = 0;

        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Mesh* mesh = GetMesh(lodLevel, i);
            if (!mesh)
            {
                continue;
            }

            totalVerts   += mesh->GetNumVertices();
            totalIndices += mesh->GetNumIndices();
            totalPolys   += mesh->GetNumPolygons();
        }

        if (outNumPolygons)
        {
            *outNumPolygons = totalPolys;
        }

        if (outNumVertices)
        {
            *outNumVertices = totalVerts;
        }

        if (outNumIndices)
        {
            *outNumIndices = totalIndices;
        }
    }


    void Actor::CalcStaticMeshTotals(uint32 lodLevel, uint32* outNumVertices, uint32* outNumIndices)
    {
        // the totals
        uint32 totalVerts   = 0;
        uint32 totalIndices = 0;

        // for all nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Mesh* mesh = GetMesh(lodLevel, i);

            // if there is no mesh at this LOD level, skip to the next node
            if (mesh == nullptr)
            {
                continue;
            }

            // the node is dynamic, and we only want static meshes, so skip to the next node
            MeshDeformerStack* stack = GetMeshDeformerStack(lodLevel, i);
            if (stack && stack->GetNumDeformers() > 0)
            {
                continue;
            }

            // sum the values to the totals
            totalVerts   += mesh->GetNumVertices();
            totalIndices += mesh->GetNumIndices();
        }

        // output the number of vertices
        if (outNumVertices)
        {
            *outNumVertices = totalVerts;
        }

        // output the number of indices
        if (outNumIndices)
        {
            *outNumIndices = totalIndices;
        }
    }


    void Actor::CalcDeformableMeshTotals(uint32 lodLevel, uint32* outNumVertices, uint32* outNumIndices)
    {
        // the totals
        uint32 totalVerts   = 0;
        uint32 totalIndices = 0;

        // for all nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Mesh* mesh = GetMesh(lodLevel, i);

            // if there is no mesh at this LOD level, skip to the next node
            if (mesh == nullptr)
            {
                continue;
            }

            // the node is not dynamic (so static), and we only want dynamic meshes, so skip to the next node
            MeshDeformerStack* stack = GetMeshDeformerStack(lodLevel, i);
            if (stack == nullptr || stack->GetNumDeformers() == 0)
            {
                continue;
            }

            // sum the values to the totals
            totalVerts   += mesh->GetNumVertices();
            totalIndices += mesh->GetNumIndices();
        }

        // output the number of vertices
        if (outNumVertices)
        {
            *outNumVertices = totalVerts;
        }

        // output the number of indices
        if (outNumIndices)
        {
            *outNumIndices = totalIndices;
        }
    }


    uint32 Actor::CalcMaxNumInfluences(uint32 lodLevel) const
    {
        uint32 maxInfluences = 0;

        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Mesh* mesh = GetMesh(lodLevel, i);
            if (!mesh)
            {
                continue;
            }

            maxInfluences = MCore::Max<uint32>(maxInfluences, mesh->CalcMaxNumInfluences());
        }

        return maxInfluences;
    }


    // verify if the skinning will look correctly in the given geometry LOD for a given skeletal LOD level
    void Actor::VerifySkinning(MCore::Array<uint8>& conflictNodeFlags, uint32 skeletalLODLevel, uint32 geometryLODLevel)
    {
        uint32 n;

        // get the number of nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();

        // check if the conflict node flag array's size is set to the number of nodes inside the actor
        if (conflictNodeFlags.GetLength() != numNodes)
        {
            conflictNodeFlags.Resize(numNodes);
        }

        // reset the conflict node array to zero which means we don't have any conflicting nodes yet
        MCore::MemSet(conflictNodeFlags.GetPtr(), 0, numNodes * sizeof(int8));

        // iterate over the all nodes in the actor
        for (n = 0; n < numNodes; ++n)
        {
            // get the current node and the pointer to the mesh for the given lod level
            Node* node = mSkeleton->GetNode(n);
            Mesh* mesh = GetMesh(geometryLODLevel, n);

            // skip nodes without meshes
            if (mesh == nullptr)
            {
                continue;
            }

            // find the skinning information, if it doesn't exist, skip to the next node
            SkinningInfoVertexAttributeLayer* skinningLayer = (SkinningInfoVertexAttributeLayer*)mesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID);
            if (skinningLayer == nullptr)
            {
                continue;
            }

            // get the number of original vertices and iterate through them
            const uint32 numOrgVerts = mesh->GetNumOrgVertices();
            for (uint32 v = 0; v < numOrgVerts; ++v)
            {
                // for all influences for this vertex
                const uint32 numInfluences = skinningLayer->GetNumInfluences(v);
                for (uint32 i = 0; i < numInfluences; ++i)
                {
                    // get the node number of the bone
                    uint32 nodeNr = skinningLayer->GetInfluence(v, i)->GetNodeNr();

                    // if the current skinning influence is linked to a node which is disabled in the given
                    // skeletal LOD we will end up with a badly skinned character, set its flag to conflict true
                    if (node->GetSkeletalLODStatus(skeletalLODLevel) == false)
                    {
                        conflictNodeFlags[nodeNr] = 1;
                    }
                }
            }
        }
    }


    uint32 Actor::CalcMaxNumInfluences(uint32 lodLevel, AZStd::vector<uint32>& outVertexCounts) const
    {
        uint32 maxInfluences = 0;

        // Reset the values.
        outVertexCounts.resize(CalcMaxNumInfluences(lodLevel) + 1);
        for (size_t k = 0; k < outVertexCounts.size(); ++k)
        {
            outVertexCounts[k] = 0;
        }

        // Get the vertex counts for the influences. (e.g. 500 vertices have 1 skinning influence, 300 vertices have 2 skinning influences etc.)
        AZStd::vector<uint32> meshVertexCounts;
        const uint32 numNodes = GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Mesh* mesh = GetMesh(lodLevel, i);
            if (!mesh)
            {
                continue;
            }

            const uint32 meshMaxInfluences = mesh->CalcMaxNumInfluences(meshVertexCounts);
            maxInfluences = MCore::Max<uint32>(maxInfluences, meshMaxInfluences);

            for (size_t j = 0; j < meshVertexCounts.size(); ++j)
            {
                outVertexCounts[j] += meshVertexCounts[j];
            }
        }

        return maxInfluences;
    }


    // check if the mesh at the given LOD is deformable
    bool Actor::CheckIfHasDeformableMesh(uint32 lodLevel) const
    {
        MCORE_ASSERT(lodLevel < mLODs.GetLength());

        // check if any of the nodes has a deformable mesh
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            if (CheckIfHasDeformableMesh(lodLevel, i))
            {
                return true;
            }
        }

        // aaaah, no deformable meshes found
        return false;
    }


    // check if there is any mesh available
    bool Actor::CheckIfHasMeshes(uint32 lodLevel) const
    {
        // check if any of the nodes has a mesh
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            if (GetMesh(lodLevel, i))
            {
                return true;
            }
        }

        // aaaah, no meshes found
        return false;
    }


    void Actor::SetPhysicsSetup(const AZStd::shared_ptr<PhysicsSetup>& physicsSetup)
    {
        m_physicsSetup = physicsSetup;
    }


    const AZStd::shared_ptr<PhysicsSetup>& Actor::GetPhysicsSetup() const
    {
        return m_physicsSetup;
    }

    const AZStd::shared_ptr<SimulatedObjectSetup>& Actor::GetSimulatedObjectSetup() const
    {
        return m_simulatedObjectSetup;
    }

    // remove all morph setups
    void Actor::RemoveAllMorphSetups(bool deleteMeshDeformers)
    {
        uint32 i;

        // get the number of lod levels
        const uint32 numLODs = GetNumLODLevels();

        // for all LODs, get rid of all the morph setups for each geometry LOD
        for (i = 0; i < mMorphSetups.GetLength(); ++i)
        {
            if (mMorphSetups[i])
            {
                mMorphSetups[i]->Destroy();
            }

            mMorphSetups[i] = nullptr;
        }

        // remove all modifiers from the stacks for each lod in all nodes
        if (deleteMeshDeformers)
        {
            // for all nodes
            const uint32 numNodes = mSkeleton->GetNumNodes();
            for (i = 0; i < numNodes; ++i)
            {
                // process all LOD levels
                for (uint32 lod = 0; lod < numLODs; ++lod)
                {
                    // if we have a modifier stack
                    MeshDeformerStack* stack = GetMeshDeformerStack(lod, i);
                    if (stack)
                    {
                        // remove all smart mesh morph deformers from that mesh deformer stack
                        stack->RemoveAllDeformersByType(MorphMeshDeformer::TYPE_ID);

                        // if there are no deformers left in the stack, remove the stack
                        if (stack->GetNumDeformers() == 0)
                        {
                            MCore::Destroy(stack);
                            SetMeshDeformerStack(lod, i, nullptr);
                        }
                    }
                }
            }
        }
    }



    // check if the material is used by the given mesh
    bool Actor::CheckIfIsMaterialUsed(Mesh* mesh, uint32 materialIndex) const
    {
        // check if the mesh is valid
        if (mesh == nullptr)
        {
            return false;
        }

        // iterate through the submeshes
        const uint32 numSubMeshes = mesh->GetNumSubMeshes();
        for (uint32 s = 0; s < numSubMeshes; ++s)
        {
            // if the submesh material index is the same as the material index we search for, then it is being used
            if (mesh->GetSubMesh(s)->GetMaterial() == materialIndex)
            {
                return true;
            }
        }

        return false;
    }


    // check if the material is used by a mesh of this actor
    bool Actor::CheckIfIsMaterialUsed(uint32 lodLevel, uint32 index) const
    {
        // iterate through all nodes of the actor and check its meshes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            // if the mesh is in LOD range check if it uses the material
            if (CheckIfIsMaterialUsed(GetMesh(lodLevel, i), index))
            {
                return true;
            }

            // same for the collision mesh
            //if (CheckIfIsMaterialUsed( GetCollisionMesh(lodLevel, i), index ))
            //return true;
        }

        // return false, this means that no mesh uses the given material
        return false;
    }


    // remove the given material and reassign all material numbers of the submeshes
    void Actor::RemoveMaterial(uint32 lodLevel, uint32 index)
    {
        MCORE_ASSERT(lodLevel < mMaterials.GetLength());

        // first of all remove the given material
        mMaterials[lodLevel].Remove(index);
    }


    // try to find the motion extraction node automatically
    Node* Actor::FindBestMotionExtractionNode() const
    {
        Node* result = nullptr;

        // the maximum number of children of a root node, the node with the most children
        // will become our repositioning node
        uint32 maxNumChilds = 0;

        // traverse through all root nodes
        const uint32 numRootNodes = mSkeleton->GetNumRootNodes();
        for (uint32 i = 0; i < numRootNodes; ++i)
        {
            // get the given root node from the actor
            Node* rootNode = mSkeleton->GetNode(mSkeleton->GetRootNodeIndex(i));

            // get the number of child nodes recursively
            const uint32 numChildNodes = rootNode->GetNumChildNodesRecursive();

            // if the number of child nodes of this node is bigger than the current max number
            // this is our new candidate for the repositioning node
            if (numChildNodes > maxNumChilds)
            {
                maxNumChilds = numChildNodes;
                result = rootNode;
            }
        }

        return result;
    }


    // automatically find and set the best motion extraction
    void Actor::AutoSetMotionExtractionNode()
    {
        SetMotionExtractionNode(FindBestMotionExtractionNode());
    }


    // extract a bone list
    void Actor::ExtractBoneList(uint32 lodLevel, MCore::Array<uint32>* outBoneList) const
    {
        // clear the existing items
        outBoneList->Clear();

        // for all nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 n = 0; n < numNodes; ++n)
        {
            Mesh* mesh = GetMesh(lodLevel, n);

            // skip nodes without meshes
            if (mesh == nullptr)
            {
                continue;
            }

            // find the skinning information, if it doesn't exist, skip to the next node
            SkinningInfoVertexAttributeLayer* skinningLayer = (SkinningInfoVertexAttributeLayer*)mesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID);
            if (skinningLayer == nullptr)
            {
                continue;
            }

            // iterate through all skinning data
            const uint32 numOrgVerts = mesh->GetNumOrgVertices();
            for (uint32 v = 0; v < numOrgVerts; ++v)
            {
                // for all influences for this vertex
                const uint32 numInfluences = skinningLayer->GetNumInfluences(v);
                for (uint32 i = 0; i < numInfluences; ++i)
                {
                    // get the node number of the bone
                    uint32 nodeNr = skinningLayer->GetInfluence(v, i)->GetNodeNr();

                    // check if it is already in the bone list, if not, add it
                    if (outBoneList->Contains(nodeNr) == false)
                    {
                        outBoneList->Add(nodeNr);
                    }
                }
            }
        }
    }


    // recursively add dependencies
    void Actor::RecursiveAddDependencies(Actor* actor)
    {
        // process all dependencies of the given actor
        const uint32 numDependencies = actor->GetNumDependencies();
        for (uint32 i = 0; i < numDependencies; ++i)
        {
            // add it to the actor instance
            mDependencies.Add(*actor->GetDependency(i));

            // recursive into the actor we are dependent on
            RecursiveAddDependencies(actor->GetDependency(i)->mActor);
        }
    }


    // remove all meshes and stacks that have no morphing on them
    uint32 Actor::RemoveAllMeshesWithoutMorphing(uint32 geomLODLevel)
    {
        uint32 numRemoved = 0;

        // for all nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            // check if the node has a morph deformer
            if (CheckIfHasMorphDeformer(geomLODLevel, i) == false) // if it hasn't got a morph deformer
            {
                // remove all meshes, and remove the mesh deformer stack for this node at the given LOD
                RemoveNodeMeshForLOD(geomLODLevel, i);

                // increase the counter
                numRemoved++;
            }
        }

        // return the number of removed meshes
        return numRemoved;
    }


    // update the bounding volumes
    void Actor::UpdateNodeBindPoseOBBs(uint32 lodLevel)
    {
        // for all nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            CalcOBBFromBindPose(lodLevel, i);
        }
    }


    // remove all node groups
    void Actor::RemoveAllNodeGroups()
    {
        const uint32 numGroups = mNodeGroups.GetLength();
        for (uint32 i = 0; i < numGroups; ++i)
        {
            mNodeGroups[i]->Destroy();
        }
        mNodeGroups.Clear();
    }


    // try to find a match for a given node with a given name
    // for example find "Bip01 L Hand" for node "Bip01 R Hand"
    uint16 Actor::FindBestMatchForNode(const char* nodeName, const char* subStringA, const char* subStringB, bool firstPass) const
    {
        char newString[1024];
        AZStd::string nameA;
        AZStd::string nameB;

        // search through all nodes to find the best match
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 n = 0; n < numNodes; ++n)
        {
            // get the node name
            const char* name = mSkeleton->GetNode(n)->GetName();

            // check if a substring appears inside this node's name
            if (strstr(name, subStringB))
            {
                // remove the substrings from the names
                nameA = nodeName;
                nameB = name;

                uint32 offset = 0;
                char* stringData = nameA.data();
                MCore::MemSet(newString, 0, 1024 * sizeof(char));
                while (offset < nameA.size())
                {
                    // locate the substring
                    stringData = strstr(stringData, subStringA);
                    if (stringData == nullptr)
                    {
                        break;
                    }

                    // replace the substring
                    // replace subStringA with subStringB
                    offset = static_cast<uint32>(stringData - nameA.data());

                    azstrncpy(newString, 1024, nameA.c_str(), offset);
                    azstrcat(newString, 1024, subStringB);
                    azstrcat(newString, 1024, stringData + strlen(subStringA));

                    stringData += strlen(subStringA);

                    // we found a match
                    if (nameB == newString)
                    {
                        return static_cast<uint16>(n);
                    }
                }
            }
        }

        if (firstPass)
        {
            return FindBestMatchForNode(nodeName, subStringB, subStringA, false); // try it the other way around (substring wise)
        }
        // return the best match
        return MCORE_INVALIDINDEX16;
    }


    // map motion source data of node 'sourceNodeName' to 'destNodeName' and the other way around
    bool Actor::MapNodeMotionSource(const char* sourceNodeName, const char* destNodeName)
    {
        // find the source node index
        const uint32 sourceNodeIndex = mSkeleton->FindNodeByNameNoCase(sourceNodeName)->GetNodeIndex();
        if (sourceNodeIndex == MCORE_INVALIDINDEX32)
        {
            return false;
        }

        // find the dest node index
        const uint32 destNodeIndex = mSkeleton->FindNodeByNameNoCase(destNodeName)->GetNodeIndex();
        if (destNodeIndex == MCORE_INVALIDINDEX32)
        {
            return false;
        }

        // allocate the data if we haven't already
        if (mNodeMirrorInfos.GetLength() == 0)
        {
            AllocateNodeMirrorInfos();
        }

        // apply the mapping
        mNodeMirrorInfos[ destNodeIndex   ].mSourceNode = static_cast<uint16>(sourceNodeIndex);
        mNodeMirrorInfos[ sourceNodeIndex ].mSourceNode = static_cast<uint16>(destNodeIndex);

        // we succeeded, because both source and dest have been found
        return true;
    }


    // map two nodes for mirroring
    bool Actor::MapNodeMotionSource(uint16 sourceNodeIndex, uint16 targetNodeIndex)
    {
        // allocate the data if we haven't already
        if (mNodeMirrorInfos.GetLength() == 0)
        {
            AllocateNodeMirrorInfos();
        }

        // apply the mapping
        mNodeMirrorInfos[ targetNodeIndex   ].mSourceNode   = static_cast<uint16>(sourceNodeIndex);
        mNodeMirrorInfos[ sourceNodeIndex ].mSourceNode     = static_cast<uint16>(targetNodeIndex);

        // we succeeded, because both source and dest have been found
        return true;
    }



    // match the node motion sources
    // substrings could be "Left " and "Right " to map the nodes "Left Hand" and "Right Hand" to eachother
    void Actor::MatchNodeMotionSources(const char* subStringA, const char* subStringB)
    {
        // try to map all nodes
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Node* node = mSkeleton->GetNode(i);

            // find the best match
            const uint16 bestIndex = FindBestMatchForNode(node->GetName(), subStringA, subStringB);

            // if a best match has been found
            if (bestIndex != MCORE_INVALIDINDEX16)
            {
                MCore::LogDetailedInfo("%s <---> %s", node->GetName(), mSkeleton->GetNode(bestIndex)->GetName());
                MapNodeMotionSource(node->GetName(), mSkeleton->GetNode(bestIndex)->GetName());
            }
        }
    }


    // set the name of the actor
    void Actor::SetName(const char* name)
    {
        mName = name;
    }


    // set the filename of the actor
    void Actor::SetFileName(const char* filename)
    {
        mFileName = filename;
    }


    // find the first active parent node in a given skeletal LOD
    uint32 Actor::FindFirstActiveParentBone(uint32 skeletalLOD, uint32 startNodeIndex) const
    {
        uint32 curNodeIndex = startNodeIndex;

        do
        {
            curNodeIndex = mSkeleton->GetNode(curNodeIndex)->GetParentIndex();
            if (curNodeIndex == MCORE_INVALIDINDEX32)
            {
                return curNodeIndex;
            }

            if (mSkeleton->GetNode(curNodeIndex)->GetSkeletalLODStatus(skeletalLOD))
            {
                return curNodeIndex;
            }
        } while (curNodeIndex != MCORE_INVALIDINDEX32);

        return MCORE_INVALIDINDEX32;
    }


    // make the geometry LOD levels compatible with the skeletal LOD levels
    // it remaps skinning influences of vertices that are linked to disabled bones, to other enabled bones
    void Actor::MakeGeomLODsCompatibleWithSkeletalLODs()
    {
        // for all geometry lod levels
        const uint32 numGeomLODs = mLODs.GetLength();
        for (uint32 geomLod = 0; geomLod < numGeomLODs; ++geomLod)
        {
            // for all nodes
            const uint32 numNodes = mSkeleton->GetNumNodes();
            for (uint32 n = 0; n < numNodes; ++n)
            {
                Node* node = mSkeleton->GetNode(n);

                // check if this node has a mesh, if not we can skip it
                Mesh* mesh = GetMesh(geomLod, n);
                if (mesh == nullptr)
                {
                    continue;
                }

                // check if the mesh is skinned, if not, we don't need to do anything
                SkinningInfoVertexAttributeLayer* layer = (SkinningInfoVertexAttributeLayer*)mesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID);
                if (layer == nullptr)
                {
                    continue;
                }

                // get shortcuts to the original vertex numbers
                const uint32* orgVertices = (uint32*)mesh->FindOriginalVertexData(Mesh::ATTRIB_ORGVTXNUMBERS);

                // for all submeshes
                const uint32 numSubMeshes = mesh->GetNumSubMeshes();
                for (uint32 s = 0; s < numSubMeshes; ++s)
                {
                    SubMesh* subMesh = mesh->GetSubMesh(s);

                    // for all vertices in the submesh
                    const uint32 startVertex = subMesh->GetStartVertex();
                    const uint32 numVertices = subMesh->GetNumVertices();
                    for (uint32 v = 0; v < numVertices; ++v)
                    {
                        const uint32 vertexIndex    = startVertex + v;
                        const uint32 orgVertex      = orgVertices[vertexIndex];

                        // for all skinning influences of the vertex
                        const uint32 numInfluences = layer->GetNumInfluences(orgVertex);
                        for (uint32 i = 0; i < numInfluences; ++i)
                        {
                            // if the bone is disabled
                            SkinInfluence* influence = layer->GetInfluence(orgVertex, i);
                            if (mSkeleton->GetNode(influence->GetNodeNr())->GetSkeletalLODStatus(geomLod) == false)
                            {
                                // find the first parent bone that is enabled in this LOD
                                const uint32 newNodeIndex = FindFirstActiveParentBone(geomLod, influence->GetNodeNr());
                                if (newNodeIndex == MCORE_INVALIDINDEX32)
                                {
                                    MCore::LogWarning("EMotionFX::Actor::MakeGeomLODsCompatibleWithSkeletalLODs() - Failed to find an enabled parent for node '%s' in skeletal LOD %d of actor '%s' (0x%x)", node->GetName(), geomLod, GetFileName(), this);
                                    continue;
                                }

                                // set the new node index
                                influence->SetNodeNr(static_cast<uint16>(newNodeIndex));
                            }
                        } // for all influences

                        // optimize the influences
                        // if they all use the same bone, just make one influence of it with weight 1.0
                        for (uint32 x = 0; x < numVertices; ++x)
                        {
                            layer->CollapseInfluences(orgVertices[startVertex + x]);
                        }
                    } // for all verts

                    // clear the bones array
                    subMesh->ReinitBonesArray(layer);
                } // for all submeshes

                // reinit the mesh deformer stacks
                MeshDeformerStack* stack = GetMeshDeformerStack(geomLod, node->GetNodeIndex());
                if (stack)
                {
                    stack->ReinitializeDeformers(this, node, geomLod);
                }
            } // for all nodes
        }
    }


    // generate a path from the current node towards the root
    void Actor::GenerateUpdatePathToRoot(uint32 endNodeIndex, MCore::Array<uint32>& outPath) const
    {
        outPath.Clear(false);
        outPath.Reserve(32);

        // start at the end effector
        Node* currentNode = mSkeleton->GetNode(endNodeIndex);
        while (currentNode)
        {
            // add the current node to the update list
            outPath.Add(currentNode->GetNodeIndex());

            // move up the hierarchy, towards the root and end node
            currentNode = currentNode->GetParentNode();
        }
    }


    // set the motion extraction node
    void Actor::SetMotionExtractionNode(Node* node)
    {
        if (node)
        {
            mMotionExtractionNode = node->GetNodeIndex();
        }
        else
        {
            mMotionExtractionNode = MCORE_INVALIDINDEX32;
        }
    }


    // set the motion extraction node
    void Actor::SetMotionExtractionNodeIndex(uint32 nodeIndex)
    {
        mMotionExtractionNode = nodeIndex;
    }


    // reinitialize all mesh deformers for all LOD levels
    void Actor::ReinitializeMeshDeformers()
    {
        const uint32 numLODLevels = GetNumLODLevels();
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Node* node = mSkeleton->GetNode(i);

            // iterate through all LOD levels
            for (uint32 lodLevel = 0; lodLevel < numLODLevels; ++lodLevel)
            {
                // reinit the mesh deformer stacks
                MeshDeformerStack* stack = GetMeshDeformerStack(lodLevel, i);
                if (stack)
                {
                    stack->ReinitializeDeformers(this, node, lodLevel);
                }
            }
        }
    }


    // post init
    void Actor::PostCreateInit(bool makeGeomLodsCompatibleWithSkeletalLODs, bool generateOBBs, bool convertUnitType)
    {
        if (mThreadIndex == MCORE_INVALIDINDEX32)
        {
            mThreadIndex = 0;
        }

        // calculate the inverse bind pose matrices
        const Pose* bindPose = GetBindPose();
        const uint32 numNodes = mSkeleton->GetNumNodes();
        mInvBindPoseTransforms.resize(numNodes);
        for (uint32 i = 0; i < numNodes; ++i)
        {
            mInvBindPoseTransforms[i] = bindPose->GetModelSpaceTransform(i).Inversed();
        }

        // make sure the skinning info doesn't use any disabled bones
        if (makeGeomLodsCompatibleWithSkeletalLODs)
        {
            MakeGeomLODsCompatibleWithSkeletalLODs();
        }

        // initialize the mesh deformers
        ReinitializeMeshDeformers();

        // make sure our world space bind pose is updated too
        if (mMorphSetups.GetLength() > 0 && mMorphSetups[0])
        {
            mSkeleton->GetBindPose()->ResizeNumMorphs(mMorphSetups[0]->GetNumMorphTargets());
        }
        mSkeleton->GetBindPose()->ForceUpdateFullModelSpacePose();
        mSkeleton->GetBindPose()->ZeroMorphWeights();

        if (generateOBBs)
        {
            UpdateNodeBindPoseOBBs(0);
        }

        // auto detect mirror axes
        if (GetHasMirrorInfo() && GetHasMirrorAxesDetected() == false)
        {
            AutoDetectMirrorAxes();
        }

        m_simulatedObjectSetup->InitAfterLoad(this);

        // build the static axis aligned bounding box by creating an actor instance (needed to perform cpu skinning mesh deforms and mesh scaling etc)
        // then copy it over to the actor
        UpdateStaticAABB();

        // rescale all content if needed
        if (convertUnitType)
        {
            ScaleToUnitType(GetEMotionFX().GetUnitType());
        }

        // post create actor
        EMotionFX::GetEventManager().OnPostCreateActor(this);
    }


    // update the static AABB (very heavy as it has to create an actor instance, update mesh deformers, calculate the mesh based bounds etc)
    void Actor::UpdateStaticAABB()
    {
        if (!mStaticAABB.CheckIfIsValid())
        {
            ActorInstance* actorInstance = ActorInstance::Create(this, nullptr, mThreadIndex);
            //actorInstance->UpdateMeshDeformers(0.0f);
            //actorInstance->UpdateStaticBasedAABBDimensions();
            actorInstance->GetStaticBasedAABB(&mStaticAABB);
            actorInstance->Destroy();
        }
    }


    // auto detect the mirror axes
    void Actor::AutoDetectMirrorAxes()
    {
        AZ::Vector3 modelSpaceMirrorPlaneNormal(1.0f, 0.0f, 0.0f);

        Pose pose;
        pose.LinkToActor(this);

        const uint32 numNodes = mNodeMirrorInfos.GetLength();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            const uint16 motionSource = (GetHasMirrorInfo()) ? GetNodeMirrorInfo(i).mSourceNode : static_cast<uint16>(i);

            // displace the local transform a bit, and calculate its mirrored model space position
            pose.InitFromBindPose(this);
            Transform localTransform = pose.GetLocalSpaceTransform(motionSource);
            Transform orgDelta;
            orgDelta.mPosition.Set(1.1f, 2.2f, 3.3f);
            orgDelta.mRotation.SetEuler(0.1f, 0.2f, 0.3f);
            Transform delta = orgDelta;
            delta.Multiply(localTransform);
            pose.SetLocalSpaceTransform(motionSource, delta);
            Transform endModelSpaceTransform = pose.GetModelSpaceTransform(motionSource);
            endModelSpaceTransform.Mirror(modelSpaceMirrorPlaneNormal);

            float   minDist     = FLT_MAX;
            uint8   bestAxis    = 0;
            uint8   bestFlags   = 0;
            bool    found       = false;
            for (uint8 a = 0; a < 3; ++a) // mirror along x, y and then z axis
            {
                AZ::Vector3 axis(0.0f, 0.0f, 0.0f);
                axis.SetElement(a, 1.0f);

                // mirror it over the current plane
                pose.InitFromBindPose(this);
                localTransform = pose.GetLocalSpaceTransform(i);
                delta = orgDelta;
                delta.Mirror(axis);
                delta.Multiply(localTransform);
                pose.SetLocalSpaceTransform(i, delta);
                const Transform& modelSpaceResult = pose.GetModelSpaceTransform(i);

                // check if we have a matching distance in model space
                const float dist = MCore::SafeLength(modelSpaceResult.mPosition - endModelSpaceTransform.mPosition);
                if (dist <= MCore::Math::epsilon)
                {
                    //MCore::LogInfo("%s = %f (axis=%d)", mNodes[i]->GetName(), dist, a);
                    mNodeMirrorInfos[i].mAxis   = a;
                    mNodeMirrorInfos[i].mFlags  = 0;
                    found = true;
                    break;
                }

                // record if this is a better match
                if (dist < minDist)
                {
                    minDist     = dist;
                    bestAxis    = a;
                    bestFlags   = 0;
                }
            }

            // try with flipped axes
            if (found == false)
            {
                for (uint8 a = 0; a < 3; ++a) // mirror along x, y and then z axis
                {
                    for (uint8 f = 0; f < 3; ++f) // flip axis
                    {
                        AZ::Vector3 axis(0.0f, 0.0f, 0.0f);
                        axis.SetElement(a, 1.0f);

                        uint8 flags = 0;
                        if (f == 0)
                        {
                            flags = MIRRORFLAG_INVERT_X;
                        }
                        if (f == 1)
                        {
                            flags = MIRRORFLAG_INVERT_Y;
                        }
                        if (f == 2)
                        {
                            flags = MIRRORFLAG_INVERT_Z;
                        }

                        // mirror it over the current plane
                        pose.InitFromBindPose(this);
                        localTransform = pose.GetLocalSpaceTransform(i);
                        delta = orgDelta;
                        delta.MirrorWithFlags(axis, flags);
                        delta.Multiply(localTransform);
                        pose.SetLocalSpaceTransform(i, delta);
                        const Transform& modelSpaceResult = pose.GetModelSpaceTransform(i);

                        // check if we have a matching distance in world space
                        const float dist = MCore::SafeLength(modelSpaceResult.mPosition - endModelSpaceTransform.mPosition);
                        if (dist <= MCore::Math::epsilon)
                        {
                            //MCore::LogInfo("*** %s = %f (axis=%d) (flip=%d)", mNodes[i]->GetName(), dist, a, f);
                            mNodeMirrorInfos[i].mAxis   = a;
                            mNodeMirrorInfos[i].mFlags  = flags;
                            found = true;
                            break;
                        }

                        // record if this is a better match
                        if (dist < minDist)
                        {
                            minDist     = dist;
                            bestAxis    = a;
                            bestFlags   = flags;
                        }
                    } // for all flips

                    if (found)
                    {
                        break;
                    }
                } // for all mirror axes
            }

            if (found == false)
            {
                mNodeMirrorInfos[i].mAxis   = bestAxis;
                mNodeMirrorInfos[i].mFlags  = bestFlags;
                //MCore::LogInfo("best for %s = %f (axis=%d) (flags=%d)", mNodes[i]->GetName(), minDist, bestAxis, bestFlags);
            }
        }

        //for (uint32 i=0; i<numNodes; ++i)
        //MCore::LogInfo("%s = (axis=%d) (flags=%d)", GetNode(i)->GetName(), mNodeMirrorInfos[i].mAxis, mNodeMirrorInfos[i].mFlags);
    }


    // get the array of node mirror infos
    const MCore::Array<Actor::NodeMirrorInfo>& Actor::GetNodeMirrorInfos() const
    {
        return mNodeMirrorInfos;
    }


    // get the array of node mirror infos
    MCore::Array<Actor::NodeMirrorInfo>& Actor::GetNodeMirrorInfos()
    {
        return mNodeMirrorInfos;
    }


    // set the node mirror infos directly
    void Actor::SetNodeMirrorInfos(const MCore::Array<NodeMirrorInfo>& mirrorInfos)
    {
        mNodeMirrorInfos = mirrorInfos;
    }


    // try to geometrically match left with right nodes
    void Actor::MatchNodeMotionSourcesGeometrical()
    {
        Pose pose;
        pose.InitFromBindPose(this);

        const uint16 numNodes = static_cast<uint16>(mSkeleton->GetNumNodes());
        for (uint16 i = 0; i < numNodes; ++i)
        {
            //Node* node = mNodes[i];

            // find the best match
            const uint16 bestIndex = FindBestMirrorMatchForNode(i, pose);

            // if a best match has been found
            if (bestIndex != MCORE_INVALIDINDEX16)
            {
                //LogDetailedInfo("%s <---> %s", node->GetName(), GetNode(bestIndex)->GetName());
                MapNodeMotionSource(i, bestIndex);
            }
        }
    }


    // find the best matching node index
    uint16 Actor::FindBestMirrorMatchForNode(uint16 nodeIndex, Pose& pose) const
    {
        if (mSkeleton->GetNode(nodeIndex)->GetIsRootNode())
        {
            return MCORE_INVALIDINDEX16;
        }

        // calculate the model space transform and mirror it
        const Transform nodeTransform       = pose.GetModelSpaceTransform(nodeIndex);
        const Transform mirroredTransform   = nodeTransform.Mirrored(AZ::Vector3(1.0f, 0.0f, 0.0f));

        uint32 numMatches = 0;
        uint16 result = MCORE_INVALIDINDEX16;

        // find nodes that have the mirrored transform
        const uint32 numNodes = mSkeleton->GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            const Transform curNodeTransform = pose.GetModelSpaceTransform(i);
            if (i != nodeIndex)
            {
                // only check the translation for now
        #ifndef EMFX_SCALE_DISABLED
                if (MCore::Compare<AZ::Vector3>::CheckIfIsClose(
                        curNodeTransform.mPosition,
                        mirroredTransform.mPosition, MCore::Math::epsilon) &&
                    MCore::Compare<float>::CheckIfIsClose(MCore::SafeLength(curNodeTransform.mScale),
                        MCore::SafeLength(mirroredTransform.mScale), MCore::Math::epsilon))
        #else
                if (MCore::Compare<AZ::Vector3>::CheckIfIsClose(curNodeTransform.mPosition, mirroredTransform.mPosition, MCore::Math::epsilon))
        #endif
                {
                    numMatches++;
                    result = static_cast<uint16>(i);
                }
            }
        }

        if (numMatches == 1)
        {
            const uint32 hierarchyDepth         = mSkeleton->CalcHierarchyDepthForNode(nodeIndex);
            const uint32 matchingHierarchyDepth = mSkeleton->CalcHierarchyDepthForNode(result);
            if (hierarchyDepth != matchingHierarchyDepth)
            {
                return MCORE_INVALIDINDEX16;
            }

            return result;
        }

        return MCORE_INVALIDINDEX16;
    }


    // resize the transform arrays to the current number of nodes
    void Actor::ResizeTransformData()
    {
        mSkeleton->GetBindPose()->LinkToActor(this, Pose::FLAG_LOCALTRANSFORMREADY, false);
        mInvBindPoseTransforms.resize(mSkeleton->GetNumNodes());
    }


    // release any transform data
    void Actor::ReleaseTransformData()
    {
        mSkeleton->GetBindPose()->Clear();
        mInvBindPoseTransforms.clear();
    }


    // copy transforms from another actor
    void Actor::CopyTransformsFrom(Actor* other)
    {
        MCORE_ASSERT(other->GetNumNodes() == mSkeleton->GetNumNodes());
        ResizeTransformData();
        mInvBindPoseTransforms = other->mInvBindPoseTransforms;
        *mSkeleton->GetBindPose() = *other->GetSkeleton()->GetBindPose();
    }


    void Actor::SetNumNodes(uint32 numNodes)
    {
        mSkeleton->SetNumNodes(numNodes);
        mNodeInfos.resize(numNodes);
        for (uint32 i = 0; i < mLODs.GetLength(); ++i)
        {
            LODLevel& lod = mLODs[i];
            lod.mNodeInfos.Resize(numNodes);
        }

        Pose* bindPose = mSkeleton->GetBindPose();
        bindPose->LinkToActor(this, Pose::FLAG_LOCALTRANSFORMREADY, false);
    }


    void Actor::AddNode(Node* node)
    {
        mSkeleton->AddNode(node);
        mSkeleton->GetBindPose()->LinkToActor(this, Pose::FLAG_LOCALTRANSFORMREADY, false);

        // initialize the LOD data
        mNodeInfos.emplace_back();
        for (uint32 i = 0; i < mLODs.GetLength(); ++i)
        {
            LODLevel& lod = mLODs[i];
            lod.mNodeInfos.AddEmpty();
        }

        mSkeleton->GetBindPose()->LinkToActor(this, Pose::FLAG_LOCALTRANSFORMREADY, false);
        Transform identTransform;
        identTransform.Identity();
        mSkeleton->GetBindPose()->SetLocalSpaceTransform(mSkeleton->GetNumNodes() - 1, identTransform);
    }


    void Actor::RemoveNode(uint32 nr, bool delMem)
    {
        mSkeleton->RemoveNode(nr, delMem);
        for (uint32 i = 0; i < mLODs.GetLength(); ++i)
        {
            mLODs[i].mNodeInfos.Remove(nr);
        }
        mNodeInfos.erase(mNodeInfos.begin() + nr);
    }


    // delete all nodes
    void Actor::DeleteAllNodes()
    {
        mSkeleton->RemoveAllNodes();
        for (uint32 i = 0; i < mLODs.GetLength(); ++i)
        {
            mLODs[i].mNodeInfos.Clear();
        }
        mNodeInfos.clear();
    }


    void Actor::ReserveMaterials(uint32 lodLevel, uint32 numMaterials)
    {
        mMaterials[lodLevel].Reserve(numMaterials);
    }


    // get a material
    Material* Actor::GetMaterial(uint32 lodLevel, uint32 nr) const
    {
        MCORE_ASSERT(lodLevel < mMaterials.GetLength());
        MCORE_ASSERT(nr < mMaterials[lodLevel].GetLength());
        return mMaterials[lodLevel][nr];
    }


    // get a material by name
    uint32 Actor::FindMaterialIndexByName(uint32 lodLevel, const char* name) const
    {
        MCORE_ASSERT(lodLevel < mMaterials.GetLength());

        // search through all materials
        const uint32 numMaterials = mMaterials[lodLevel].GetLength();
        for (uint32 i = 0; i < numMaterials; ++i)
        {
            if (mMaterials[lodLevel][i]->GetNameString() == name)
            {
                return i;
            }
        }

        // no material found
        return MCORE_INVALIDINDEX32;
    }


    // set a material
    void Actor::SetMaterial(uint32 lodLevel, uint32 nr, Material* mat)
    {
        MCORE_ASSERT(lodLevel < mMaterials.GetLength());
        MCORE_ASSERT(nr < mMaterials[lodLevel].GetLength());
        mMaterials[lodLevel][nr] = mat;
    }


    // add a material
    void Actor::AddMaterial(uint32 lodLevel, Material* mat)
    {
        MCORE_ASSERT(lodLevel < mMaterials.GetLength());
        mMaterials[lodLevel].Add(mat);
    }


    // get the number of materials
    uint32 Actor::GetNumMaterials(uint32 lodLevel) const
    {
        MCORE_ASSERT(lodLevel < mMaterials.GetLength());
        return mMaterials[lodLevel].GetLength();
    }


    uint32 Actor::GetNumLODLevels() const
    {
        return mLODs.GetLength();
    }


    void* Actor::GetCustomData() const
    {
        return mCustomData;
    }


    void Actor::SetCustomData(void* dataPointer)
    {
        mCustomData = dataPointer;
    }


    const char* Actor::GetName() const
    {
        return mName.c_str();
    }


    const AZStd::string& Actor::GetNameString() const
    {
        return mName;
    }


    const char* Actor::GetFileName() const
    {
        return mFileName.c_str();
    }


    const AZStd::string& Actor::GetFileNameString() const
    {
        return mFileName;
    }


    void Actor::AddDependency(const Dependency& dependency)
    {
        mDependencies.Add(dependency);
    }


    void Actor::SetMorphSetup(uint32 lodLevel, MorphSetup* setup)
    {
        mMorphSetups[lodLevel] = setup;
    }


    uint32 Actor::GetNumNodeGroups() const
    {
        return mNodeGroups.GetLength();
    }


    NodeGroup* Actor::GetNodeGroup(uint32 index) const
    {
        return mNodeGroups[index];
    }


    void Actor::AddNodeGroup(NodeGroup* newGroup)
    {
        mNodeGroups.Add(newGroup);
    }


    void Actor::RemoveNodeGroup(uint32 index, bool delFromMem)
    {
        if (delFromMem)
        {
            mNodeGroups[index]->Destroy();
        }

        mNodeGroups.Remove(index);
    }


    void Actor::RemoveNodeGroup(NodeGroup* group, bool delFromMem)
    {
        mNodeGroups.RemoveByValue(group);
        if (delFromMem)
        {
            group->Destroy();
        }
    }


    // find a group index by its name
    uint32 Actor::FindNodeGroupIndexByName(const char* groupName) const
    {
        const uint32 numGroups = mNodeGroups.GetLength();
        for (uint32 i = 0; i < numGroups; ++i)
        {
            if (mNodeGroups[i]->GetNameString() == groupName)
            {
                return i;
            }
        }

        return MCORE_INVALIDINDEX32;
    }


    // find a group index by its name, but not case sensitive
    uint32 Actor::FindNodeGroupIndexByNameNoCase(const char* groupName) const
    {
        const uint32 numGroups = mNodeGroups.GetLength();
        for (uint32 i = 0; i < numGroups; ++i)
        {
            if (AzFramework::StringFunc::Equal(mNodeGroups[i]->GetNameString().c_str(), groupName, false /* no case */))
            {
                return i;
            }
        }

        return MCORE_INVALIDINDEX32;
    }


    // find a group by its name
    NodeGroup* Actor::FindNodeGroupByName(const char* groupName) const
    {
        const uint32 numGroups = mNodeGroups.GetLength();
        for (uint32 i = 0; i < numGroups; ++i)
        {
            if (mNodeGroups[i]->GetNameString() == groupName)
            {
                return mNodeGroups[i];
            }
        }
        return nullptr;
    }


    // find a group by its name, but without case sensitivity
    NodeGroup* Actor::FindNodeGroupByNameNoCase(const char* groupName) const
    {
        const uint32 numGroups = mNodeGroups.GetLength();
        for (uint32 i = 0; i < numGroups; ++i)
        {
            if (AzFramework::StringFunc::Equal(mNodeGroups[i]->GetNameString().c_str(), groupName, false /* no case */))
            {
                return mNodeGroups[i];
            }
        }
        return nullptr;
    }


    void Actor::SetDirtyFlag(bool dirty)
    {
        mDirtyFlag = dirty;
    }


    bool Actor::GetDirtyFlag() const
    {
        return mDirtyFlag;
    }


    void Actor::SetIsUsedForVisualization(bool flag)
    {
        mUsedForVisualization = flag;
    }


    bool Actor::GetIsUsedForVisualization() const
    {
        return mUsedForVisualization;
    }

    void Actor::SetIsOwnedByRuntime(bool isOwnedByRuntime)
    {
#if defined(EMFX_DEVELOPMENT_BUILD)
        mIsOwnedByRuntime = isOwnedByRuntime;
#else
        AZ_UNUSED(isOwnedByRuntime);
#endif
    }


    bool Actor::GetIsOwnedByRuntime() const
    {
#if defined(EMFX_DEVELOPMENT_BUILD)
        return mIsOwnedByRuntime;
#else
        return true;
#endif
    }


    const MCore::AABB& Actor::GetStaticAABB() const
    {
        return mStaticAABB;
    }


    void Actor::SetStaticAABB(const MCore::AABB& box)
    {
        mStaticAABB = box;
    }

    //---------------------------------

    // set the mesh for a given node in a given LOD
    void Actor::SetMesh(uint32 lodLevel, uint32 nodeIndex, Mesh* mesh)
    {
        mLODs[lodLevel].mNodeInfos[nodeIndex].mMesh = mesh;
    }


    // set the mesh deformer stack for a given node in a given LOD
    void Actor::SetMeshDeformerStack(uint32 lodLevel, uint32 nodeIndex, MeshDeformerStack* stack)
    {
        mLODs[lodLevel].mNodeInfos[nodeIndex].mStack = stack;
    }


    // check if the mesh is deformable
    bool Actor::CheckIfHasDeformableMesh(uint32 lodLevel, uint32 nodeIndex) const
    {
        const NodeLODInfo& nodeInfo = mLODs[lodLevel].mNodeInfos[nodeIndex];
        if (nodeInfo.mMesh == nullptr)
        {
            return false;
        }

        return (nodeInfo.mStack && nodeInfo.mStack->GetNumDeformers() > 0);
    }


    // check if the mesh at the given LOD has a morph deformer
    bool Actor::CheckIfHasMorphDeformer(uint32 lodLevel, uint32 nodeIndex) const
    {
        // check if there is a mesh
        Mesh* mesh = GetMesh(lodLevel, nodeIndex);
        if (mesh == nullptr)
        {
            return false;
        }

        // check if there is a mesh deformer stack
        MeshDeformerStack* stack = GetMeshDeformerStack(lodLevel, nodeIndex);
        if (stack == nullptr)
        {
            return false;
        }

        // check if there is a morph deformer on the stack
        return stack->CheckIfHasDeformerOfType(MorphMeshDeformer::TYPE_ID);
    }


    // check if the mesh has a skinning deformer (either linear or dual quat)
    bool Actor::CheckIfHasSkinningDeformer(uint32 lodLevel, uint32 nodeIndex) const
    {
        // check if there is a mesh
        Mesh* mesh = GetMesh(lodLevel, nodeIndex);
        if (!mesh)
        {
            return false;
        }

        // check if there is a mesh deformer stack
        MeshDeformerStack* stack = GetMeshDeformerStack(lodLevel, nodeIndex);
        if (!stack)
        {
            return false;
        }

        return (stack->CheckIfHasDeformerOfType(SoftSkinDeformer::TYPE_ID) || stack->CheckIfHasDeformerOfType(DualQuatSkinDeformer::TYPE_ID));
    }


    // calculate the OBB for a given node
    void Actor::CalcOBBFromBindPose(uint32 lodLevel, uint32 nodeIndex)
    {
        AZStd::vector<AZ::Vector3> points;

        // if there is a mesh
        Mesh* mesh = GetMesh(lodLevel, nodeIndex);
        if (mesh)
        {
            // if the mesh is not skinned
            if (mesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID) == nullptr)
            {
                mesh->ExtractOriginalVertexPositions(points);
            }
        }
        else // there is no mesh, so maybe this is a bone
        {
            const Transform& invBindPoseTransform = GetInverseBindPoseTransform(nodeIndex);

            // for all nodes inside the actor where this node belongs to
            const uint32 numNodes = mSkeleton->GetNumNodes();
            for (uint32 n = 0; n < numNodes; ++n)
            {
                Mesh* loopMesh = GetMesh(lodLevel, n);
                if (loopMesh == nullptr)
                {
                    continue;
                }

                // get the vertex positions in bind pose
                const uint32 numVerts = loopMesh->GetNumVertices();
                points.reserve(numVerts * 2);
                AZ::PackedVector3f* positions = (AZ::PackedVector3f*)loopMesh->FindOriginalVertexData(Mesh::ATTRIB_POSITIONS);
                //AZ::Vector3* positions = (AZ::Vector3*)loopMesh->FindOriginalVertexData(Mesh::ATTRIB_POSITIONS);

                SkinningInfoVertexAttributeLayer* skinLayer = (SkinningInfoVertexAttributeLayer*)loopMesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID);
                if (skinLayer)
                {
                    // iterate over all skinning influences and see if this node number is used
                    // if so, add it to the list of points
                    const uint32* orgVertices = (uint32*)loopMesh->FindVertexData(Mesh::ATTRIB_ORGVTXNUMBERS);
                    for (uint32 v = 0; v < numVerts; ++v)
                    {
                        // get the original vertex number
                        const uint32 orgVtx = orgVertices[v];

                        // for all skinning influences for this vertex
                        const uint32 numInfluences = skinLayer->GetNumInfluences(orgVtx);
                        for (uint32 i = 0; i < numInfluences; ++i)
                        {
                            // get the node used by this influence
                            const uint32 nodeNr = skinLayer->GetInfluence(orgVtx, i)->GetNodeNr();

                            // if this is the same node as we are updating the bounds for, add the vertex position to the list
                            if (nodeNr == nodeIndex)
                            {
                                const AZ::Vector3 tempPos(positions[v]);
                                points.emplace_back(invBindPoseTransform.TransformPoint(tempPos));
                            }
                        } // for all influences
                    } // for all vertices
                } // if there is skinning info
            } // for all nodes
        }

        // init from the set of points
        if (!points.empty())
        {
            GetNodeOBB(nodeIndex).InitFromPoints(&points[0], static_cast<uint32>(points.size()));
        }
        else
        {
            GetNodeOBB(nodeIndex).Init();
        }
    }


    // remove the mesh for a given node in a given LOD
    void Actor::RemoveNodeMeshForLOD(uint32 lodLevel, uint32 nodeIndex, bool destroyMesh)
    {
        LODLevel&       lod         = mLODs[lodLevel];
        NodeLODInfo&    nodeInfo    = lod.mNodeInfos[nodeIndex];

        if (destroyMesh && nodeInfo.mMesh)
        {
            MCore::Destroy(nodeInfo.mMesh);
        }

        if (destroyMesh && nodeInfo.mStack)
        {
            MCore::Destroy(nodeInfo.mStack);
        }

        nodeInfo.mMesh  = nullptr;
        nodeInfo.mStack = nullptr;
    }


    bool Actor::GetHasMesh(uint32 lodLevel, uint32 nodeIndex) const
    {
        return (mLODs[lodLevel].mNodeInfos[nodeIndex].mMesh != nullptr);
    }


    void Actor::SetUnitType(MCore::Distance::EUnitType unitType)
    {
        mUnitType = unitType;
    }


    MCore::Distance::EUnitType Actor::GetUnitType() const
    {
        return mUnitType;
    }


    void Actor::SetFileUnitType(MCore::Distance::EUnitType unitType)
    {
        mFileUnitType = unitType;
    }


    MCore::Distance::EUnitType Actor::GetFileUnitType() const
    {
        return mFileUnitType;
    }


    // scale all data
    void Actor::Scale(float scaleFactor)
    {
        // if we don't need to adjust the scale, do nothing
        if (MCore::Math::IsFloatEqual(scaleFactor, 1.0f))
        {
            return;
        }

        // scale the bind pose positions
        Pose* bindPose = GetBindPose();
        const uint32 numNodes = GetNumNodes();
        for (uint32 i = 0; i < numNodes; ++i)
        {
            Transform transform = bindPose->GetLocalSpaceTransform(i);
            transform.mPosition *= scaleFactor;
            bindPose->SetLocalSpaceTransform(i, transform);
        }
        bindPose->ForceUpdateFullModelSpacePose();

        // calculate the inverse bind pose matrices
        for (uint32 i = 0; i < numNodes; ++i)
        {
            mInvBindPoseTransforms[i] = bindPose->GetModelSpaceTransform(i).Inversed();
        }

        // update node obbs
        for (uint32 i = 0; i < numNodes; ++i)
        {
            MCore::OBB& box = GetNodeOBB(i);
            box.SetExtents(box.GetExtents() * scaleFactor);
            box.SetCenter(box.GetCenter() * scaleFactor);
        }

        // update static aabb
        mStaticAABB.SetMin(mStaticAABB.GetMin() * scaleFactor);
        mStaticAABB.SetMax(mStaticAABB.GetMax() * scaleFactor);

        // update mesh data for all LOD levels
        const uint32 numLODs = GetNumLODLevels();
        for (uint32 lod = 0; lod < numLODs; ++lod)
        {
            for (uint32 i = 0; i < numNodes; ++i)
            {
                Mesh* mesh = GetMesh(lod, i);
                if (mesh)
                {
                    mesh->Scale(scaleFactor);
                }
            }
        }

        // scale morph target data
        for (uint32 lod = 0; lod < numLODs; ++lod)
        {
            MorphSetup* morphSetup = GetMorphSetup(lod);
            if (morphSetup)
            {
                morphSetup->Scale(scaleFactor);
            }
        }

        // initialize the mesh deformers just to be sure
        ReinitializeMeshDeformers();

        // trigger the event
        GetEventManager().OnScaleActorData(this, scaleFactor);
    }


    // scale everything to the given unit type
    void Actor::ScaleToUnitType(MCore::Distance::EUnitType targetUnitType)
    {
        if (mUnitType == targetUnitType)
        {
            return;
        }

        // calculate the scale factor and scale
        const float scaleFactor = static_cast<float>(MCore::Distance::GetConversionFactor(mUnitType, targetUnitType));
        Scale(scaleFactor);

        // update the unit type
        mUnitType = targetUnitType;
    }


    // Try to figure out which axis points "up" for the motion extraction node.
    Actor::EAxis Actor::FindBestMatchingMotionExtractionAxis() const
    {
        MCORE_ASSERT(mMotionExtractionNode != MCORE_INVALIDINDEX32);
        if (mMotionExtractionNode == MCORE_INVALIDINDEX32)
        {
            return AXIS_Y;
        }

        // Get the local space rotation matrix of the motion extraction node.
        const Transform& localTransform = GetBindPose()->GetLocalSpaceTransform(mMotionExtractionNode);
        const MCore::Matrix rotationMatrix = localTransform.mRotation.ToMatrix();

        // Calculate angles between the up axis and each of the rotation's basis vectors.
        const AZ::Vector3 globalUpAxis(0.0f, 0.0f, 1.0f);
        const float dotX = rotationMatrix.GetRow(0).Dot(globalUpAxis);
        const float dotY = rotationMatrix.GetRow(1).Dot(globalUpAxis);
        const float dotZ = rotationMatrix.GetRow(2).Dot(globalUpAxis);

        const float difX = 1.0f - MCore::Clamp(MCore::Math::Abs(dotX), 0.0f, 1.0f);
        const float difY = 1.0f - MCore::Clamp(MCore::Math::Abs(dotY), 0.0f, 1.0f);
        const float difZ = 1.0f - MCore::Clamp(MCore::Math::Abs(dotZ), 0.0f, 1.0f);

        // Pick the axis which has the smallest angle difference.
        if (difX <= difY && difY <= difZ)
        {
            return AXIS_X;
        }
        else if (difY <= difX && difX <= difZ)
        {
            return AXIS_Y;
        }
        else
        {
            return AXIS_Z;
        }
    }


    void Actor::SetRetargetRootNodeIndex(uint32 nodeIndex)
    {
        mRetargetRootNode = nodeIndex;
    }


    void Actor::SetRetargetRootNode(Node* node)
    {
        mRetargetRootNode = node ? node->GetNodeIndex() : MCORE_INVALIDINDEX32;
    }
} // namespace EMotionFX
