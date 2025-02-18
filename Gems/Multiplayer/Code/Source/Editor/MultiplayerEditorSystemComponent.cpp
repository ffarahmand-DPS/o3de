/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Multiplayer/IMultiplayer.h>
#include <Multiplayer/IMultiplayerTools.h>
#include <Multiplayer/INetworkSpawnableLibrary.h>
#include <Multiplayer/MultiplayerConstants.h>

#include <MultiplayerSystemComponent.h>
#include <Editor/MultiplayerEditorSystemComponent.h>
#include <Source/AutoGen/Multiplayer.AutoPackets.h>

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Console/ILogger.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Utils/Utils.h>
#include <AzNetworking/Framework/INetworking.h>
#include <AzToolsFramework/Entity/PrefabEditorEntityOwnershipInterface.h>

namespace Multiplayer
{
    using namespace AzNetworking;

    AZ_CVAR(bool, editorsv_enabled, false, nullptr, AZ::ConsoleFunctorFlags::DontReplicate,
        "Whether Editor launching a local server to connect to is supported");
    AZ_CVAR(bool, editorsv_launch, true, nullptr, AZ::ConsoleFunctorFlags::DontReplicate,
        "Whether Editor should launch a server when the server address is localhost");
    AZ_CVAR(AZ::CVarFixedString, editorsv_process, "", nullptr, AZ::ConsoleFunctorFlags::DontReplicate,
        "The server executable that should be run. Empty to use the current project's ServerLauncher");
    AZ_CVAR(AZ::CVarFixedString, editorsv_serveraddr, AZ::CVarFixedString(LocalHost), nullptr, AZ::ConsoleFunctorFlags::DontReplicate, "The address of the server to connect to");
    AZ_CVAR(uint16_t, editorsv_port, DefaultServerEditorPort, nullptr, AZ::ConsoleFunctorFlags::DontReplicate, "The port that the multiplayer editor gem will bind to for traffic");

    void MultiplayerEditorSystemComponent::Reflect(AZ::ReflectContext* context)
    {
        if (AZ::SerializeContext* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<MultiplayerEditorSystemComponent, AZ::Component>()
                ->Version(1);
        }
    }

    void MultiplayerEditorSystemComponent::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required)
    {
        required.push_back(AZ_CRC_CE("MultiplayerService"));
    }

    void MultiplayerEditorSystemComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("MultiplayerEditorService"));
    }

    void MultiplayerEditorSystemComponent::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible)
    {
        incompatible.push_back(AZ_CRC_CE("MultiplayerEditorService"));
    }

    MultiplayerEditorSystemComponent::MultiplayerEditorSystemComponent()
        : m_serverAcceptanceReceivedHandler([this](){OnServerAcceptanceReceived();})
    {
        ;
    }

    void MultiplayerEditorSystemComponent::Activate()
    {
        AzFramework::GameEntityContextEventBus::Handler::BusConnect();
        AzToolsFramework::EditorEvents::Bus::Handler::BusConnect();
        MultiplayerEditorServerRequestBus::Handler::BusConnect();
        AZ::Interface<IMultiplayer>::Get()->AddServerAcceptanceReceivedHandler(m_serverAcceptanceReceivedHandler);
    }

    void MultiplayerEditorSystemComponent::Deactivate()
    {
        AzToolsFramework::EditorEvents::Bus::Handler::BusDisconnect();
        AzFramework::GameEntityContextEventBus::Handler::BusDisconnect();
        MultiplayerEditorServerRequestBus::Handler::BusDisconnect();
    }

    void MultiplayerEditorSystemComponent::NotifyRegisterViews()
    {
        AZ_Assert(m_editor == nullptr, "NotifyRegisterViews occurred twice!");
        m_editor = nullptr;
        AzToolsFramework::EditorRequests::Bus::BroadcastResult(m_editor, &AzToolsFramework::EditorRequests::GetEditor);
        m_editor->RegisterNotifyListener(this);
    }

    void MultiplayerEditorSystemComponent::OnEditorNotifyEvent(EEditorNotifyEvent event)
    {
        switch (event)
        {
        case eNotify_OnQuit:
            AZ_Warning("Multiplayer Editor", m_editor != nullptr, "Multiplayer Editor received On Quit without an Editor pointer.");
            if (m_editor)
            {
                m_editor->UnregisterNotifyListener(this);
                m_editor = nullptr;
            }
            [[fallthrough]];
        case eNotify_OnEndGameMode:
            // Kill the configured server if it's active
            if (m_serverProcess)
            {
                m_serverProcess->TerminateProcess(0);
                m_serverProcess = nullptr;
            }

            if (INetworkInterface* editorNetworkInterface = AZ::Interface<INetworking>::Get()->RetrieveNetworkInterface(AZ::Name(MpEditorInterfaceName)))
            {
                editorNetworkInterface->Disconnect(m_editorConnId, AzNetworking::DisconnectReason::TerminatedByClient);
            }
            if (auto console = AZ::Interface<AZ::IConsole>::Get(); console)
            {
                console->PerformCommand("disconnect");
            }

            AZ::Interface<INetworkEntityManager>::Get()->ClearAllEntities();

            // Rebuild the library to clear temporary in-memory spawnable assets
            AZ::Interface<INetworkSpawnableLibrary>::Get()->BuildSpawnablesList();

            break;
        }
    }

    AzFramework::ProcessWatcher* LaunchEditorServer()
    {
        // Assemble the server's path
        AZ::CVarFixedString serverProcess = editorsv_process;
        AZ::IO::FixedMaxPath serverPath;
        if (serverProcess.empty())
        {
            // If enabled but no process name is supplied, try this project's ServerLauncher
            serverProcess = AZ::Utils::GetProjectName() + ".ServerLauncher";
            serverPath = AZ::Utils::GetExecutableDirectory();
            serverPath /= serverProcess + AZ_TRAIT_OS_EXECUTABLE_EXTENSION;
        }
        else
        {
            serverPath = serverProcess;
        }

        // Start the configured server if it's available
        AzFramework::ProcessLauncher::ProcessLaunchInfo processLaunchInfo;
        processLaunchInfo.m_commandlineParameters = AZStd::string::format(
            R"("%s" --project-path "%s" --editorsv_isDedicated true --sv_defaultPlayerSpawnAsset "%s")",
            serverPath.c_str(),
            AZ::Utils::GetProjectPath().c_str(),
            static_cast<AZ::CVarFixedString>(sv_defaultPlayerSpawnAsset).c_str());
        processLaunchInfo.m_showWindow = true;
        processLaunchInfo.m_processPriority = AzFramework::ProcessPriority::PROCESSPRIORITY_NORMAL;

        // Launch the Server
        AzFramework::ProcessWatcher* outProcess = AzFramework::ProcessWatcher::LaunchProcess(
            processLaunchInfo, AzFramework::ProcessCommunicationType::COMMUNICATOR_TYPE_NONE);

        return outProcess;
    }

    void MultiplayerEditorSystemComponent::OnGameEntitiesStarted()
    {
        auto prefabEditorEntityOwnershipInterface = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!prefabEditorEntityOwnershipInterface)
        {
            AZ_Error("MultiplayerEditor", prefabEditorEntityOwnershipInterface != nullptr, "PrefabEditorEntityOwnershipInterface unavailable");
        }

        // BeginGameMode and Prefab Processing have completed at this point
        IMultiplayerTools* mpTools = AZ::Interface<IMultiplayerTools>::Get();
        if (editorsv_enabled && mpTools != nullptr)
        {
            const AZStd::vector<AZ::Data::Asset<AZ::Data::AssetData>>& assetData = prefabEditorEntityOwnershipInterface->GetPlayInEditorAssetData();
        
            AZStd::vector<uint8_t> buffer;
            AZ::IO::ByteContainerStream byteStream(&buffer);

            // Serialize Asset information and AssetData into a potentially large buffer
            for (const auto& asset : assetData)
            {
                AZ::Data::AssetId assetId = asset.GetId();
                AZStd::string assetHint = asset.GetHint();
                uint32_t hintSize = aznumeric_cast<uint32_t>(assetHint.size());

                byteStream.Write(sizeof(AZ::Data::AssetId), reinterpret_cast<void*>(&assetId));
                byteStream.Write(sizeof(uint32_t), reinterpret_cast<void*>(&hintSize));
                byteStream.Write(assetHint.size(), assetHint.data());
                AZ::Utils::SaveObjectToStream(byteStream, AZ::DataStream::ST_BINARY, asset.GetData(), asset.GetData()->GetType());
            }

            const AZ::CVarFixedString remoteAddress = editorsv_serveraddr;
            if (editorsv_launch)
            {
                if (LocalHost != remoteAddress)
                {
                    AZ_Warning(
                        "MultiplayerEditor", false,
                        "Launching EditorServer skipped because incompatible cvars. editorsv_launch=true, meaning you want to launch an editor-server on this machine, but the editorsv_serveraddr is %s instead of the local address (127.0.0.1). "
                        "Please either set editorsv_launch=false and keep the remote editor-server, or set editorsv_launch=true and editorsv_serveraddr=127.0.0.1.",
                        remoteAddress.c_str())
                    return;
                }
                
                // Begin listening for MPEditor packets before we launch the editor-server.
                // The editor-server will send us (the editor) an "EditorServerReadyForLevelData" packet to let us know it's ready to receive data.
                INetworkInterface* editorNetworkInterface =
                    AZ::Interface<INetworking>::Get()->RetrieveNetworkInterface(AZ::Name(MpEditorInterfaceName));
                AZ_Assert(editorNetworkInterface, "MP Editor Network Interface was unregistered before Editor could connect.");
                editorNetworkInterface->Listen(editorsv_port);

                // Launch the editor-server
                m_serverProcess = LaunchEditorServer();
            }
            else
            {
                // Editorsv_launch=false, so we're expecting an editor-server already exists.
                // Connect to the editor-server and then send the EditorServerLevelData packet.
                INetworkInterface* editorNetworkInterface = AZ::Interface<INetworking>::Get()->RetrieveNetworkInterface(AZ::Name(MpEditorInterfaceName));
                AZ_Assert(editorNetworkInterface, "MP Editor Network Interface was unregistered before Editor could connect.")
                
                m_editorConnId = editorNetworkInterface->Connect(AzNetworking::IpAddress(remoteAddress.c_str(), editorsv_port, AzNetworking::ProtocolType::Tcp));

                if (m_editorConnId == AzNetworking::InvalidConnectionId)
                {
                    AZ_Warning(
                        "MultiplayerEditor", false,
                        "Editor multiplayer game-mode failed! Could not connect to an editor-server. editorsv_launch is false so we're assuming you're running your own editor-server at editorsv_serveraddr(%s) on editorsv_port(%i). "
                        "Either set editorsv_launch=true so the editor launches an editor-server for you, or launch your own editor-server by hand before entering game-mode. Remember editor-servers must use editorsv_isDedicated=true.",
                        remoteAddress.c_str(),
                        static_cast < uint16_t>(editorsv_port))
                    return;
                }

                SendEditorServerLevelDataPacket(editorNetworkInterface->GetConnectionSet().GetConnection(m_editorConnId));
            }
        }
    }

    void MultiplayerEditorSystemComponent::OnGameEntitiesReset()
    {
    }

    void MultiplayerEditorSystemComponent::OnServerAcceptanceReceived()
    {
        // We're now accepting the connection to the EditorServer.
        // In normal game clients SendReadyForEntityUpdates will be enabled once the appropriate level's root spawnable is loaded,
        // but since we're in Editor, we're already in the level.
        AZ::Interface<IMultiplayer>::Get()->SendReadyForEntityUpdates(true);
    }

    void MultiplayerEditorSystemComponent::SendEditorServerLevelDataPacket(AzNetworking::IConnection* connection)
    {
        const auto prefabEditorEntityOwnershipInterface = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!prefabEditorEntityOwnershipInterface)
        {
            AZ_Error("MultiplayerEditor", prefabEditorEntityOwnershipInterface != nullptr, "PrefabEditorEntityOwnershipInterface unavailable")
            return;
        }

        const AZStd::vector<AZ::Data::Asset<AZ::Data::AssetData>>& assetData = prefabEditorEntityOwnershipInterface->GetPlayInEditorAssetData();

        AZStd::vector<uint8_t> buffer;
        AZ::IO::ByteContainerStream byteStream(&buffer);

        // Serialize Asset information and AssetData into a potentially large buffer
        for (const auto& asset : assetData)
        {
            AZ::Data::AssetId assetId = asset.GetId();
            AZStd::string assetHint = asset.GetHint();
            auto hintSize = aznumeric_cast<uint32_t>(assetHint.size());

            byteStream.Write(sizeof(AZ::Data::AssetId), reinterpret_cast<void*>(&assetId));
            byteStream.Write(sizeof(uint32_t), reinterpret_cast<void*>(&hintSize));
            byteStream.Write(assetHint.size(), assetHint.data());
            AZ::Utils::SaveObjectToStream(byteStream, AZ::DataStream::ST_BINARY, asset.GetData(), asset.GetData()->GetType());
        }

        // Spawnable library needs to be rebuilt since now we have newly registered in-memory spawnable assets
        AZ::Interface<INetworkSpawnableLibrary>::Get()->BuildSpawnablesList();

        // Read the buffer into EditorServerLevelData packets until we've flushed the whole thing
        byteStream.Seek(0, AZ::IO::GenericStream::SeekMode::ST_SEEK_BEGIN);

        while (byteStream.GetCurPos() < byteStream.GetLength())
        {
            MultiplayerEditorPackets::EditorServerLevelData editorServerLevelDataPacket;
            auto& outBuffer = editorServerLevelDataPacket.ModifyAssetData();

            // Size the packet's buffer appropriately
            size_t readSize = outBuffer.GetCapacity();
            const size_t byteStreamSize = byteStream.GetLength() - byteStream.GetCurPos();
            if (byteStreamSize < readSize)
            {
                readSize = byteStreamSize;
            }

            outBuffer.Resize(readSize);
            byteStream.Read(readSize, outBuffer.GetBuffer());

            // If we've run out of buffer, mark that we're done
            if (byteStream.GetCurPos() == byteStream.GetLength())
            {
                editorServerLevelDataPacket.SetLastUpdate(true);
            }

            connection->SendReliablePacket(editorServerLevelDataPacket);
        }
    }

}
