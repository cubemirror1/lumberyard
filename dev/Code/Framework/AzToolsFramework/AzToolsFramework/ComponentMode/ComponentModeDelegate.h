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

#include <AzCore/Memory/SystemAllocator.h>
#include <AzToolsFramework/API/ComponentEntitySelectionBus.h>
#include <AzToolsFramework/ComponentMode/EditorComponentModeBus.h>
#include <AzToolsFramework/ToolsComponents/EditorLockComponentBus.h>
#include <AzToolsFramework/ToolsComponents/EditorVisibilityBus.h>

namespace AzToolsFramework
{
    namespace ComponentModeFramework
    {
        /// Utility factory function to create a ComponentModeBuilder for a specific EditorComponent
        template<typename EditorComponentType, typename EditorComponentModeType>
        ComponentModeBuilder CreateComponentModeBuilder(const AZ::EntityComponentIdPair& entityComponentIdPair)
        {
            const auto componentModeBuilderFunc = [entityComponentIdPair]()
            {
                return AZStd::make_unique<EditorComponentModeType>(
                    entityComponentIdPair, AZ::AzTypeInfo<EditorComponentType>::Uuid());
            };

            return ComponentModeBuilder(
                entityComponentIdPair.GetComponentId(), AZ::AzTypeInfo<EditorComponentType>::Uuid(), componentModeBuilderFunc);
        }

        /// Helper to provide ComponentMode button in the Entity Inspector and double click
        /// handling in the viewport for entering/exiting ComponentMode.
        class ComponentModeDelegate
            : private ComponentModeDelegateRequestBus::Handler
            , private EntitySelectionEvents::Bus::Handler
            , private EditorEntityVisibilityNotificationBus::Handler
            , private EditorEntityLockComponentNotificationBus::Handler
        {
        public:
            /// @cond
            AZ_CLASS_ALLOCATOR(ComponentModeDelegate, AZ::SystemAllocator, 0);
            AZ_RTTI(ComponentModeDelegate, "{635B28F0-601A-43D2-A42A-02C4A88CD9C2}");

            static void Reflect(AZ::ReflectContext* context);
            /// @endcond

            /// Connect the ComponentModeDelegate to listen for Editor selection events.
            /// Editor Component must call Connect (or variant of Connect), usually in Component::Activate, and
            /// Disconnect, most likely in Component::Deactivate.
            template<typename EditorComponentType>
            void Connect(
                const AZ::EntityComponentIdPair& entityComponentIdPair, EditorComponentSelectionRequestsBus::Handler* handler)
            {
                ConnectInternal(entityComponentIdPair, AZ::AzTypeInfo<EditorComponentType>::Uuid(), handler);
            }

            /// Connect the ComponentModeDelegate to listen for Editor selection events and
            /// simultaneously add a single concrete ComponentMode (common case utility).
            template<typename EditorComponentType, typename EditorComponentModeType>
            void ConnectWithSingleComponentMode(
                const AZ::EntityComponentIdPair& entityComponentIdPair, EditorComponentSelectionRequestsBus::Handler* handler)
            {
                Connect<EditorComponentType>(entityComponentIdPair, handler);
                IndividualComponentMode<EditorComponentType, EditorComponentModeType>();
            }

            /// Disconnect the ComponentModeDelegate to stop listening for Editor selection events.
            void Disconnect();

            /// Has this specific ComponentModeDelegate (for a specific Entity and Component)
            /// been added to ComponentMode.
            bool AddedToComponentMode();
            /// The function to call when this ComponentModeDelegate detects an event to enter ComponentMode.
            void SetAddComponentModeCallback(
                const AZStd::function<void(const AZ::EntityComponentIdPair&)>& addComponentModeCallback);

        private:
            void ConnectInternal(
                const AZ::EntityComponentIdPair& entityComponentIdPair, AZ::Uuid componentType,
                EditorComponentSelectionRequestsBus::Handler* handler);

            /// Utility function for the common case of creating a single ComponentMode for a Component.
            template<typename EditorComponentType, typename EditorComponentModeType>
            void IndividualComponentMode()
            {
                SetAddComponentModeCallback([](const AZ::EntityComponentIdPair& entityComponentIdPair)
                {
                    const auto componentModeBuilder =
                        CreateComponentModeBuilder<EditorComponentType, EditorComponentModeType>(entityComponentIdPair);

                    const auto entityAndComponentModeBuilder =
                        EntityAndComponentModeBuilders(entityComponentIdPair.GetEntityId(), componentModeBuilder);

                    ComponentModeSystemRequestBus::Broadcast(
                        &ComponentModeSystemRequests::AddComponentModes, entityAndComponentModeBuilder);
                });
            }

            void AddComponentMode();

            // EntitySelectionEvents
            void OnSelected() override;
            void OnDeselected() override;

            // ComponentMouseViewportRequestBus
            bool DetectEnterComponentModeInteraction(
                const ViewportInteraction::MouseInteractionEvent& mouseInteraction) override;
            bool DetectLeaveComponentModeInteraction(
                const ViewportInteraction::MouseInteractionEvent& mouseInteraction) override;
            void AddComponentModeOfType(AZ::Uuid componentType) override;

            // EditorEntityVisibilityNotificationBus
            void OnEntityVisibilityChanged(bool visibility) override;

            // EditorEntityLockComponentNotificationBus
            void OnEntityLockChanged(bool locked) override;

            /// Is the ComponentMode button active/operational.
            /// It will not be if the entity with this component is either locked or hidden.
            bool ComponentModeButtonInactive() const;

            AZ::Uuid m_componentType; ///< The type of component entering ComponentMode.
            AZ::EntityComponentIdPair m_entityComponentIdPair; ///< The Entity and Component Id this ComponentMode is bound to.
            EditorComponentSelectionRequestsBus::Handler* m_handler = nullptr; /**< Selection handler (used for double clicking
                                                                                 *  on a component to enter ComponentMode). */
            AZStd::function<void(const AZ::EntityComponentIdPair&)> m_addComponentMode; ///< Callback to add ComponentMode for this component.

            /// ComponentMode Button
            void OnComponentModeEnterButtonPressed();
            void OnComponentModeLeaveButtonPressed();
            bool m_componentModeEnterButton = false;
            bool m_componentModeLeaveButton = false;
        };
    } // namespace ComponentModeFramework
} // namespace AzToolsFramework