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

#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>

#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzFramework/Physics/World.h>
#include <PhysXDebug/PhysXDebugBus.h>

#include <Cry_Camera.h>
#include <IRenderAuxGeom.h>

#include <AzFramework/Physics/SystemBus.h>

#ifdef IMGUI_ENABLED
#include <imgui/imgui.h>
#include <ImGuiBus.h>
#endif // #ifdef IMGUI_ENABLED

namespace PhysXDebug
{
    struct PhysXvisualizationSettings
    {
        AZ_RTTI(PhysXvisualizationSettings, "{A3A03872-36A3-44AB-B0A9-29F709E8E3B0}");

        virtual ~PhysXvisualizationSettings() = default;

        bool m_visualizationEnabled = false;
        bool m_visualizeCollidersByProximity = false;

        // physx culling only applied to: eCOLLISION_SHAPES, eCOLLISION_EDGES and eCOLLISION_FNORMALS (eCOLLISION_AABBS are not culled by PhysX!)
        // see: \PhysX_3.4\Source\PhysX\src\NpShapeManager.cpp
        float m_scale = 1.0f;
        bool m_collisionShapes = true;
        bool m_collisionEdges = true;
        bool m_collisionFNormals = false;

        // the remaining properties will start *disable*
        bool m_collisionAabbs = false;
        bool m_collisionAxes = false;
        bool m_collisionCompounds = false;
        bool m_collisionStatic = false;
        bool m_collisionDynamic = false;

        bool m_bodyAxes = false;
        bool m_bodyMassAxes = false;
        bool m_bodyLinVelocity = false;
        bool m_bodyAngVelocity = false;

        bool m_contactPoint = false;
        bool m_contactNormal = false;

        bool m_jointLocalFrames = false;
        bool m_jointLimits = false;

        bool m_mbpRegions = false;
        bool m_actorAxes = false;

        /// Determine if the PhysX Debug Gem Visualization is currently enabled (for the editor context)
        inline bool IsPhysXDebugEnabled() { return m_visualizationEnabled; };
    };

    struct Culling
    {
        AZ_RTTI(Culling, "{20727A63-4FF7-4F31-B6F5-7FEFCB7CB153}");

        virtual ~Culling() = default;

        bool m_enabled = true;
        bool m_boxWireframe = false;
        float m_boxSize = 35.0f;
    };

    struct ColorMappings
    {
        AZ_RTTI(ColorMappings, "{021E40A6-568E-430A-9332-EF180DACD3C0}");
        // user defined colors for physx debug primitives
        ColorB m_defaultColor;
        ColorB m_black;
        ColorB m_red;
        ColorB m_green;
        ColorB m_blue;
        ColorB m_yellow;
        ColorB m_magenta;
        ColorB m_cyan;
        ColorB m_white;
        ColorB m_grey;
        ColorB m_darkRed;
        ColorB m_darkGreen;
        ColorB m_darkBlue;
    };

    class SystemComponent
        : public AZ::Component
        , protected PhysXDebugRequestBus::Handler
        , public PhysXDebugRequestBus
        , public AZ::TickBus::Handler
        , public CrySystemEventBus::Handler
#ifdef IMGUI_ENABLED
        , public ImGui::ImGuiUpdateListenerBus::Handler
#endif // IMGUI_ENABLED
#ifdef PHYSXDEBUG_GEM_EDITOR
        , public Physics::SystemNotificationBus::Handler
#endif // PHYSXDEBUG_GEM_EDITOR
    {
    public:

        AZ_COMPONENT(SystemComponent, "{111041CE-4C75-48E0-87C3-20938C05B9E0}");

        static void Reflect(AZ::ReflectContext* context);

        static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided);
        static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible);
        static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required);
        static void GetDependentServices(AZ::ComponentDescriptor::DependencyArrayType& dependent);

        // PhysxDebugDraw::PhysxDebugDrawRequestBus
        void SetVisualization(bool enabled) override;
        void ToggleVisualizationConfiguration() override;
        void SetCullingBoxSize(float cullingBoxSize) override;
        void ToggleCullingWireFrame() override;
        void ToggleColliderProximityDebugVisualization() override;

#ifdef IMGUI_ENABLED
        void OnImGuiMainMenuUpdate() override;
#endif // IMGUI_ENABLED

    protected:

        // AZ::Component interface implementation
        void Activate() override;
        void Deactivate() override;

        // TickBus::Handler
        void OnTick(float deltaTime, AZ::ScriptTimePoint time) override;
        int GetTickOrder() override { return AZ::ComponentTickBus::TICK_FIRST + 1; }

        // CrySystemEvents
        void OnCrySystemInitialized(ISystem&, const SSystemInitParams&) override;

#ifdef PHYSXDEBUG_GEM_EDITOR
        // Physics::SystemNotificationBus
        void OnPostPhysicsUpdate(float, Physics::World*) override;
#endif

    private:

        /// Configure a PhysX scene debug visualization properties.
        void ConfigurePhysXVisualizationParameters();

        /// Convert from PhysX Visualization debug colors to user defined colors.
        /// @param originalColor a color from the PhysX debug visualization data.
        /// @return a user specified color mapping (defaulting to the original PhysX color).
        ColorB MapOriginalPhysXColorToUserDefinedValues(const physx::PxU32& originalColor);

        /// Initialise the PhysX debug draw colors based on defaults.
        void InitPhysXColorMappings();

        /// Register debug drawing PhysX commands with Lumberyard console during game mode.
        void RegisterCommands();

        /// Draw the culling box being used by the viewport.
        /// @param cullingBoxAabb culling box Aabb to debug draw.
        void DrawDebugCullingBox(const AZ::Aabb& cullingBoxAabb);

        /// Configure primary debug draw settings for PhysX
        /// @param context the reflect context to utilize.
        static void ReflectPhysXDebugSettings(AZ::ReflectContext* context);

        /// Configure a culling box for PhysX visualization from the active camera.
        void ConfigureCullingBox();

        /// Gather visualization lines for this scene.
        void GatherLines(const physx::PxRenderBuffer& rb);

        /// Gather visualization triangles for this scene.
        void GatherTriangles(const physx::PxRenderBuffer& rb);

        /// Gather Joint Limits.
        void GatherJointLimits();

        /// Helper functions to wrap buffer management functionality.
        void ClearBuffers();
        void GatherBuffers();
        void RenderBuffers();

        /// Updates PhysX preferences to perform collider visualization based on proximity to camera.
        void UpdateColliderVisualizationByProximity();

#ifdef IMGUI_ENABLED
        /// Build a specific color picker menu option.
        void BuildColorPickingMenuItem(const AZStd::string& label, ColorB& color);
#endif // IMGUI_ENABLED

        Physics::World* GetCurrentPhysicsWorld();

        // Main configuration
        PhysXvisualizationSettings m_settings;
        Culling m_culling;
        ColorMappings m_colorMappings;
        AZ::ScriptTimePoint m_currentTime;
        bool m_registered = false;
        physx::PxBounds3 m_cullingBox;
        bool m_editorPhysicsWorldDirty = true;
        static const float m_maxCullingBoxSize;

        AZStd::vector<Vec3> m_linePoints;
        AZStd::vector<ColorB> m_lineColors;
        AZStd::vector<Vec3> m_trianglePoints;
        AZStd::vector<ColorB> m_triangleColors;

        // joint limit buffers
        AZStd::vector<AZ::Vector3> m_jointVertexBuffer;
        AZStd::vector<AZ::u32> m_jointIndexBuffer;
        AZStd::vector<AZ::Vector3> m_jointLineBuffer;
        AZStd::vector<bool> m_jointLineValidityBuffer;
    };

    /// Possible console parameters for physx_Debug cvar.
    enum class DebugCVarValues : uint8_t
    {
        Disable, ///< Disable debug visualization.
        Enable, ///< Enable debug visualization.
        SwitchConfigurationPreference, ///< Switch between basic and full visualization configuration.
        ColliderProximityDebug ///< Toggle visualize collision shapes by proximity to camera in editor mode.
    };
}
