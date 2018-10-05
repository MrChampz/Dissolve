#include "Application.h"

Application::Application(HINSTANCE hInst)
	: D3DApp(hInst)
{

}

Application::~Application()
{
	if (m_d3dDevice != nullptr)
	{
		FlushCommandQueue();
	}
}

bool Application::Initialize()
{
	// Change the back buffer format to a better HDR to SDR output.
	m_BackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

	if (!D3DApp::Initialize())
	{
		return false;
	}

	// Enable MSAA.
	SetMSAAState(true);

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(m_CommandList->Reset(m_DirectCmdListAlloc.Get(), nullptr));

	m_BlurFilter = std::make_unique<BlurFilter>(
		m_d3dDevice.Get(), m_ClientWidth, m_ClientHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);

	m_OffscreenRTs[0] = std::make_unique<RenderTarget>(
		m_d3dDevice.Get(), m_ClientWidth, m_ClientHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
	m_OffscreenRTs[1] = std::make_unique<RenderTarget>(
		m_d3dDevice.Get(), m_ClientWidth, m_ClientHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);

	LoadTextures();
	BuildBunnyGeometry();
	BuildMaterials();
	BuildRenderItems();
	BuildFrameResources();

	BuildDescriptorHeaps();
	BuildRootSignature();
	BuildPostProcessRootSignature();
	BuildShadersAndInputLayout();
	BuildPSOs();

	// Set the MSAA state of the offscreen render targets
	// as the same of the backbuffer offscreen render target.
	m_OffscreenRTs[0]->SetMSAAState(true, m_msaaLevel, m_msaaQuality);
	m_OffscreenRTs[1]->SetMSAAState(true, m_msaaLevel, m_msaaQuality);

	// Execute the initialization commands.
	ThrowIfFailed(m_CommandList->Close());
	ID3D12CommandList* cmdLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void Application::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +3 descriptors, one for MSAA offscreen render target.
	// And the other two for application offscreen render targets.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;

	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc,
		IID_PPV_ARGS(m_RtvHeap.GetAddressOf())));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;

	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc,
		IID_PPV_ARGS(m_DsvHeap.GetAddressOf())));
}

void Application::OnResize()
{
	D3DApp::OnResize();

	// The window resized, so update the aspect ratio and recompute the projection matrix.
	XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, GetAspectRatio(), 1.0f, 1000.0f);
	XMStoreFloat4x4(&m_Proj, proj);

	if (m_BlurFilter != nullptr)
	{
		m_BlurFilter->OnResize(m_ClientWidth, m_ClientHeight);
	}

	if (m_OffscreenRTs[0] != nullptr)
	{
		m_OffscreenRTs[0]->OnResize(m_ClientWidth, m_ClientHeight);
	}

	if (m_OffscreenRTs[1] != nullptr)
	{
		m_OffscreenRTs[1]->OnResize(m_ClientWidth, m_ClientHeight);
	}
}

void Application::Update(const GameTimer& timer)
{
	OnKeyboardInput(timer);
	UpdateCamera(timer);

	// Cycle through the circular frame resource array.
	m_CurrFrameResourceIndex = (m_CurrFrameResourceIndex + 1) % g_NumFrameResources;
	m_CurrFrameResource = m_FrameResources[m_CurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (m_CurrFrameResource->Fence != 0 && m_Fence->GetCompletedValue() < m_CurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(m_Fence->SetEventOnCompletion(m_CurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	UpdateObjectCBs(timer);
	UpdateMaterialCBs(timer);
	UpdateMainPassCB(timer);
}

void Application::Draw(const GameTimer& timer)
{
	auto cmdListAlloc = m_CurrFrameResource->CmdListAlloc;
	// Reuse the memory associated with command recording.
	// We can only reset when the command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandLists.
	// Reusing the command list reuses memory.
	ThrowIfFailed(m_CommandList->Reset(cmdListAlloc.Get(), m_PSOs["Opaque"].Get()));

	// Set the viewport and scissor rect. This needs to be reset whenever the command list is reset.
	m_CommandList->RSSetViewports(1, &m_ScreenViewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

	// Indicate a state transition on the offscreen render targets.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_OffscreenRTs[0]->GetResource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_OffscreenRTs[1]->GetResource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

	XMVECTORF32 color;
	color.v = XMColorSRGBToRGB(Colors::Black);

	// Clear the render targets and depth buffer.
	m_CommandList->ClearRenderTargetView(m_OffscreenRTs[0]->GetRtv(), color, 0, nullptr);
	m_CommandList->ClearRenderTargetView(m_OffscreenRTs[1]->GetRtv(), color, 0, nullptr);
	m_CommandList->ClearDepthStencilView(GetDepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> RTVs =
	{
		m_OffscreenRTs[0]->GetRtv(),
		m_OffscreenRTs[1]->GetRtv()
	};
	m_CommandList->OMSetRenderTargets(RTVs.size(), RTVs.data(), true, &GetDepthStencilView());

	/*
	if (m_msaaState)
	{
		// Indicate a state transition on the offscreen resource usage.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetOffscreenBuffer(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear the back buffer and depth buffer.
		m_CommandList->ClearRenderTargetView(GetOffscreenBufferView(), Colors::Black, 0, nullptr);
		m_CommandList->ClearDepthStencilView(GetDepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		// Specify the buffers we are going to render to.
		m_CommandList->OMSetRenderTargets(1, &GetOffscreenBufferView(), true, &GetDepthStencilView());
	}
	else
	{
		// Indicate a state transition on resource usage.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetCurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear the back buffer and depth buffer.
		m_CommandList->ClearRenderTargetView(GetCurrentBackBufferView(), Colors::Black, 0, nullptr);
		m_CommandList->ClearDepthStencilView(GetDepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		// Specify the buffers we are going to render to.
		m_CommandList->OMSetRenderTargets(1, &GetCurrentBackBufferView(), true, &GetDepthStencilView());
	}
	*/

	// Bind the descriptor heaps.
	ID3D12DescriptorHeap* descriptorHeaps[] = { m_SrvDescriptorHeap.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	// Bind the root signature.
	m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	// Bind per-pass constant buffer. We only need to this once per-pass.
	auto passCB = m_CurrFrameResource->PassCB->GetResource();
	m_CommandList->SetGraphicsRootConstantBufferView(3, passCB->GetGPUVirtualAddress());

	// Draw opaque render items.
	DrawRenderItems(m_CommandList.Get(), m_RenderableLayer[(int)RenderLayer::Opaque]);

	// Execute blur filtering.
	m_BlurFilter->Execute(
		m_CommandList.Get(),
		m_PostProcessRootSignature.Get(),
		m_PSOs["HorzBlur"].Get(),
		m_PSOs["VertBlur"].Get(),
		m_OffscreenRTs[1]->GetResource(),
		4);

	// Transition the first offscreen buffer's state back to GENERIC_READ.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_OffscreenRTs[0]->GetResource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

	// Transition the second offscreen buffer's state back to GENERIC_READ.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_OffscreenRTs[1]->GetResource(),
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));

	// Indicate a state transition on the offscreen MSAA render target resource usage.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetOffscreenBuffer(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Specify the buffers we are going to render to.
	m_CommandList->OMSetRenderTargets(1, &GetOffscreenBufferView(), true, &GetDepthStencilView());

	// Render the post processing.
	m_CommandList->SetGraphicsRootSignature(m_PostProcessRootSignature.Get());
	m_CommandList->SetPipelineState(m_PSOs["Composite"].Get());
	m_CommandList->SetGraphicsRootDescriptorTable(0, m_OffscreenRTs[0]->GetSrv());
	m_CommandList->SetGraphicsRootDescriptorTable(1, m_BlurFilter->GetOutputSrv());
	DrawFullScreenQuad(m_CommandList.Get());

	// Prepare the buffers to MSAA resolve.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetOffscreenBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetCurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST));

	// Perform the resolve between the offscreen render target and the actual swapchain render target.
	m_CommandList->ResolveSubresource(GetCurrentBackBuffer(), 0, GetOffscreenBuffer(), 0, m_BackBufferFormat);

	// Change the MSAA offscreen render target's state back to GENERIC_READ.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetOffscreenBuffer(),
		D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));

	// Indicate a state transition on the back buffer resource usage.
	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetCurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT));

	/*
	if (m_msaaState)
	{
		// Transition the offscreen buffer state to RESOLVE_SOURCE.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			GetOffscreenBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE));

		// Transition the current swapchain buffer state to RESOLVE_DEST.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			GetCurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST));

		// Perform the resolve between the offscreen render target and the actual swapchain render target.
		m_CommandList->ResolveSubresource(GetCurrentBackBuffer(), 0, GetOffscreenBuffer(), 0, m_BackBufferFormat);

		// Transition the offscreen buffer state back to GENERIC_READ.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetOffscreenBuffer(),
			D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));

		// Indicate a state transition on the resource usage.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetCurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT));
	}
	else
	{
		// Indicate a state transition on the resource usage.
		m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(GetCurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
	}
	*/

	// Done recording commands.
	ThrowIfFailed(m_CommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

	// Swap the back and front buffers.
	ThrowIfFailed(m_SwapChain->Present(1, 0));
	m_CurrentBackBuffer = (m_CurrentBackBuffer + 1) % SwapChainBufferCount;

	// Advance the fence value to mark commands up to this fence point.
	m_CurrFrameResource->Fence = ++m_CurrentFence;

	// Add an instruction to the command queue to set a new fence point.
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFence);
}

void Application::OnMouseDown(WPARAM btnState, int x, int y)
{
	m_LastMousePos.x = m_InitialMousePos.x = x;
	m_LastMousePos.y = m_InitialMousePos.y = y;

	SetCapture(m_AppWnd);
	ShowCursor(false);
}

void Application::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
	ClientToScreen(m_AppWnd, &m_InitialMousePos);
	SetCursorPos(m_InitialMousePos.x, m_InitialMousePos.y);
	ShowCursor(true);
}

void Application::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - m_LastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - m_LastMousePos.y));

		// Update angles based on input to orbit camera around box.
		m_Theta += dx;
		m_Phi += dy;

		// Restrict the angle m_Phi.
		m_Phi = MathHelper::Clamp(m_Phi, 0.1f, MathHelper::Pi - 0.1f);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		// Make each pixel correspond to 0.2 unit in the scene.
		float dx = 0.05f * static_cast<float>(x - m_LastMousePos.x);
		float dy = 0.05f * static_cast<float>(y - m_LastMousePos.y);

		// Update the camera radius based on input.
		m_Radius += dx - dy;

		// Restrict the radius.
		m_Radius = MathHelper::Clamp(m_Radius, 5.0f, 150.0f);
	}

	m_LastMousePos.x = x;
	m_LastMousePos.y = y;
}

void Application::OnKeyboardInput(const GameTimer& timer)
{

}

void Application::UpdateCamera(const GameTimer& timer)
{
	// Convert Spherical to Cartesian coordinates.
	m_EyePos.x = m_Radius * sinf(m_Phi) * cosf(m_Theta);
	m_EyePos.z = m_Radius * sinf(m_Phi) * sinf(m_Theta);
	m_EyePos.y = m_Radius * cosf(m_Phi);

	// Build the view matrix.
	XMVECTOR pos = XMVectorSet(m_EyePos.x, m_EyePos.y, m_EyePos.z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&m_View, view);
}

void Application::UpdateObjectCBs(const GameTimer& timer)
{
	auto currObjectCB = m_CurrFrameResource->ObjectCB.get();
	for (auto& item : m_AllRenderables)
	{
		// Only update the cbuffer data if the constants have changed.
		// This needs to be tracked per frame resource.
		if (item->NumFrameDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&item->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&item->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

			currObjectCB->CopyData(item->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			item->NumFrameDirty--;
		}
	}
}

void Application::UpdateMaterialCBs(const GameTimer& timer)
{
	auto currMaterialCB = m_CurrFrameResource->MaterialCB.get();
	for (auto& e : m_Materials)
	{
		// Only update the cbuffer if the constants have changed. If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));

			currMaterialCB->CopyData(mat->MatCBIndex, matConstants);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void Application::UpdateMainPassCB(const GameTimer& timer)
{
	XMMATRIX view = XMLoadFloat4x4(&m_View);
	XMMATRIX proj = XMLoadFloat4x4(&m_Proj);
	XMMATRIX viewProj = XMMatrixMultiply(view, proj);

	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&m_MainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_MainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_MainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&m_MainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&m_MainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&m_MainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));

	m_MainPassCB.EyePosW = m_EyePos;
	m_MainPassCB.RenderTargetSize = XMFLOAT2((float)m_ClientWidth, (float)m_ClientHeight);
	m_MainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / m_ClientWidth, 1.0f / m_ClientHeight);
	m_MainPassCB.NearZ = 1.0f;
	m_MainPassCB.FarZ = 1000.0f;
	m_MainPassCB.TotalTime = timer.TotalTime();
	m_MainPassCB.DeltaTime = timer.DeltaTime();

	m_MainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
	m_MainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	m_MainPassCB.Lights[0].Strength = { 0.6f, 0.6f, 0.6f };
	m_MainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	m_MainPassCB.Lights[1].Strength = { 0.4f, 0.4f, 0.4f };
	m_MainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	m_MainPassCB.Lights[2].Strength = { 0.2f, 0.2f, 0.2f };

	auto currPassCB = m_CurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, m_MainPassCB);
}

void Application::LoadTextures()
{
	auto stone = std::make_unique<Texture>();
	stone->Name = "stone";
	stone->Filename = L"Textures/stone.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(m_d3dDevice.Get(),
		m_CommandList.Get(), stone->Filename.c_str(),
		stone->Resource, stone->UploadHeap));

	m_Textures[stone->Name] = std::move(stone);
}

void Application::BuildBunnyGeometry()
{
	GeometryGenerator::MeshData bunny = GeometryGenerator::LoadModel("Models/bunny.asset");

	std::vector<Vertex> vertices(bunny.Vertices.size());
	std::vector<std::uint32_t> indices(bunny.Indices32);

	for (size_t i = 0; i < bunny.Vertices.size(); ++i)
	{
		vertices[i].Pos      = bunny.Vertices[i].Position;
		vertices[i].Normal   = bunny.Vertices[i].Normal;

		XMVECTOR P = XMLoadFloat3(&bunny.Vertices[i].Position);

		// Project point onto unit sphere and generate spherical texture coordinates.
		XMFLOAT3 spherePos;
		XMStoreFloat3(&spherePos, XMVector3Normalize(P));

		float theta = atan2f(spherePos.z, spherePos.x);
		float phi = acosf(spherePos.y);

		// Put in [0, 2pi] range.
		if (theta < 0.0f)
			theta += XM_2PI;

		float u = theta / (2.0f * XM_PI);
		float v = phi / XM_PI;

		vertices[i].TexCoord = { u, v };
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "bunnyGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = D3DUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
		m_CommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = D3DUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
		m_CommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubMeshGeometry submesh;
	submesh.IndexCount = (UINT)bunny.Indices32.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;
	geo->DrawArgs["bunny"] = submesh;

	m_Geometries[geo->Name] = std::move(geo);
}

void Application::BuildMaterials()
{
	auto stone = std::make_unique<Material>();
	stone->Name = "stone";
	stone->MatCBIndex = 0;
	stone->DiffuseSrvHeapIndex = 0;
	stone->DiffuseAlbedo = XMFLOAT4(0.4862f, 0.4862f, 0.4862f, 1.0f);
	stone->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
	stone->Roughness = 0.2f;

	m_Materials["stone"] = std::move(stone);
}

void Application::BuildRenderItems()
{
	auto bunny = std::make_unique<Renderable>();
	bunny->ObjCBIndex = 0;
	bunny->Mat = m_Materials["stone"].get();
	bunny->Geo = m_Geometries["bunnyGeo"].get();
	bunny->PrimitiveType = D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	bunny->IndexCount = bunny->Geo->DrawArgs["bunny"].IndexCount;
	bunny->StartIndexLocation = bunny->Geo->DrawArgs["bunny"].StartIndexLocation;
	bunny->BaseVertexLocation = bunny->Geo->DrawArgs["bunny"].BaseVertexLocation;
	m_RenderableLayer[(int)RenderLayer::Opaque].push_back(bunny.get());

	m_AllRenderables.push_back(std::move(bunny));
}

void Application::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<Renderable*>& renderables)
{
	UINT objCBByteSize = D3DUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT matCBByteSize = D3DUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

	auto objCB = m_CurrFrameResource->ObjectCB->GetResource();
	auto matCB = m_CurrFrameResource->MaterialCB->GetResource();

	// For each render item..
	for (size_t i = 0; i < renderables.size(); ++i)
	{
		auto item = renderables[i];

		cmdList->IASetVertexBuffers(0, 1, &item->Geo->GetVertexBufferView());
		cmdList->IASetIndexBuffer(&item->Geo->GetIndexBufferView());
		cmdList->IASetPrimitiveTopology(item->PrimitiveType);

		CD3DX12_GPU_DESCRIPTOR_HANDLE tex(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		tex.Offset(item->Mat->DiffuseSrvHeapIndex, m_CbvSrvUavDescriptorSize);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objCB->GetGPUVirtualAddress() + item->ObjCBIndex * objCBByteSize;
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + item->Mat->MatCBIndex * matCBByteSize;

		cmdList->SetGraphicsRootDescriptorTable(0, tex);
		cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
		cmdList->SetGraphicsRootConstantBufferView(2, matCBAddress);

		cmdList->DrawIndexedInstanced(item->IndexCount, 1, item->StartIndexLocation, item->BaseVertexLocation, 0);
	}
}

void Application::DrawFullScreenQuad(ID3D12GraphicsCommandList* cmdList)
{
	// Null-out IA stage since we build the vertex off the SV_VertexID in the shader.
	cmdList->IASetVertexBuffers(0, 1, nullptr);
	cmdList->IASetIndexBuffer(nullptr);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	cmdList->DrawInstanced(3, 1, 0, 0);
}

void Application::BuildFrameResources()
{
	for (int i = 0; i < g_NumFrameResources; ++i)
	{
		m_FrameResources.push_back(std::make_unique<FrameResource>(m_d3dDevice.Get(),
			1, (UINT)m_AllRenderables.size(), (UINT)m_Materials.size()));
	}
}

void Application::BuildDescriptorHeaps()
{
	const int textDescCount = 1;
	const int blurDescCount = 4;
	const int ofRTDescCount = 2;

	// Create the SRV heap.
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = textDescCount + blurDescCount + ofRTDescCount;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvDescriptorHeap)));

	// Fill out the heap with actual descriptors.
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	auto stone   = m_Textures["stone"]->Resource;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = -1;

	srvDesc.Format = stone->GetDesc().Format;
	m_d3dDevice->CreateShaderResourceView(stone.Get(), &srvDesc, hDescriptor);

	//
	//	Fill out the heap with the descriptors to the Blur resources.
	//

	int offset = textDescCount;

	m_BlurFilter->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), offset, m_CbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), offset, m_CbvSrvUavDescriptorSize),
		m_CbvSrvUavDescriptorSize);

	//
	//	Fill out the heap with the descriptors to the offscreen RT resources.
	//

	int rtvOffset = SwapChainBufferCount + 1;
	offset += blurDescCount;

	m_OffscreenRTs[0]->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), offset, m_CbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), offset, m_CbvSrvUavDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), rtvOffset, m_RtvDescriptorSize));
	
	rtvOffset += 1;
	offset += 1;

	m_OffscreenRTs[1]->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), offset, m_CbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), offset, m_CbvSrvUavDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), rtvOffset, m_RtvDescriptorSize));
}

void Application::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];

	// Create root CBVs.
	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[1].InitAsConstantBufferView(0);
	slotRootParameter[2].InitAsConstantBufferView(1);
	slotRootParameter[3].InitAsConstantBufferView(2);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// Create a root signature with a single slot which points
	// to a descriptor range consisting of a single constant buffer.
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;

	HRESULT result = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(result);

	ThrowIfFailed(m_d3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_RootSignature.GetAddressOf())));
}

void Application::BuildPostProcessRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE srvTable0;
	srvTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE srvTable1;
	srvTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

	CD3DX12_DESCRIPTOR_RANGE uavTable0;
	uavTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];

	// Performance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsDescriptorTable(1, &srvTable0);
	slotRootParameter[1].InitAsDescriptorTable(1, &srvTable1);
	slotRootParameter[2].InitAsDescriptorTable(1, &uavTable0);
	slotRootParameter[3].InitAsConstants(12, 0);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
		_countof(slotRootParameter), slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// Create the root signature.
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(m_d3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_PostProcessRootSignature.GetAddressOf())));
}

void Application::BuildShadersAndInputLayout()
{
	m_Shaders["StandardVS"]  = D3DUtil::LoadBinary(L"Shaders/Standard_VS.cso");
	m_Shaders["OpaquePS"]    = D3DUtil::LoadBinary(L"Shaders/Opaque_PS.cso");
	m_Shaders["HorzBlurCS"]  = D3DUtil::LoadBinary(L"Shaders/HorzBlur_CS.cso");
	m_Shaders["VertBlurCS"]  = D3DUtil::LoadBinary(L"Shaders/VertBlur_CS.cso");
	m_Shaders["CompositeVS"] = D3DUtil::LoadBinary(L"Shaders/Composite_VS.cso");
	m_Shaders["CompositePS"] = D3DUtil::LoadBinary(L"Shaders/Composite_PS.cso");

	m_InputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void Application::BuildPSOs()
{
	//
	// PSO for opaque objects.
	//

	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = {};
	opaquePsoDesc.InputLayout = { m_InputLayout.data(), (UINT)m_InputLayout.size() };
	opaquePsoDesc.pRootSignature = m_RootSignature.Get();
	opaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(m_Shaders["StandardVS"]->GetBufferPointer()),
		m_Shaders["StandardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(m_Shaders["OpaquePS"]->GetBufferPointer()),
		m_Shaders["OpaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 2;
	opaquePsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	opaquePsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	opaquePsoDesc.DSVFormat = m_DepthStencilFormat;
	opaquePsoDesc.SampleDesc.Count = m_msaaState ? m_msaaLevel : 1;
	opaquePsoDesc.SampleDesc.Quality = m_msaaState ? (m_msaaQuality - 1) : 0;

	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&m_PSOs["Opaque"])));

	//
	// PSO for compositing post process.
	//

	D3D12_GRAPHICS_PIPELINE_STATE_DESC compositePsoDesc = {};
	compositePsoDesc.InputLayout = { m_InputLayout.data(), (UINT)m_InputLayout.size() };
	compositePsoDesc.pRootSignature = m_PostProcessRootSignature.Get();
	compositePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(m_Shaders["CompositeVS"]->GetBufferPointer()),
		m_Shaders["CompositeVS"]->GetBufferSize()
	};
	compositePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(m_Shaders["CompositePS"]->GetBufferPointer()),
		m_Shaders["CompositePS"]->GetBufferSize()
	};
	compositePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	compositePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	compositePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	compositePsoDesc.DepthStencilState.DepthEnable = false;
	compositePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	compositePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	compositePsoDesc.SampleMask = UINT_MAX;
	compositePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	compositePsoDesc.NumRenderTargets = 1;
	compositePsoDesc.RTVFormats[0] = m_BackBufferFormat;
	compositePsoDesc.DSVFormat = m_DepthStencilFormat;
	compositePsoDesc.SampleDesc.Count = m_msaaState ? m_msaaLevel : 1;
	compositePsoDesc.SampleDesc.Quality = m_msaaState ? (m_msaaQuality - 1) : 0;

	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&compositePsoDesc, IID_PPV_ARGS(&m_PSOs["Composite"])));

	//
	// PSO for horizontal blur
	//

	D3D12_COMPUTE_PIPELINE_STATE_DESC horzBlurPSO = {};
	horzBlurPSO.pRootSignature = m_PostProcessRootSignature.Get();
	horzBlurPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	horzBlurPSO.CS =
	{
		reinterpret_cast<BYTE*>(m_Shaders["HorzBlurCS"]->GetBufferPointer()),
		m_Shaders["HorzBlurCS"]->GetBufferSize()
	};

	ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(&horzBlurPSO, IID_PPV_ARGS(&m_PSOs["HorzBlur"])));

	//
	// PSO for vertical blur
	//

	D3D12_COMPUTE_PIPELINE_STATE_DESC vertBlurPSO = {};
	vertBlurPSO.pRootSignature = m_PostProcessRootSignature.Get();
	vertBlurPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	vertBlurPSO.CS =
	{
		reinterpret_cast<BYTE*>(m_Shaders["VertBlurCS"]->GetBufferPointer()),
		m_Shaders["VertBlurCS"]->GetBufferSize()
	};

	ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(&vertBlurPSO, IID_PPV_ARGS(&m_PSOs["VertBlur"])));
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> Application::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers. So just define them all up front
	// and keep them available as part of the root signature.

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap
	(
		0,									// shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT,		// filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP		// addressW
	);

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp
	(
		1,									// shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT,		// filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP		// addressW
	);

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap
	(
		2,									// shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,	// filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP		// addressW
	);

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp
	(
		3,									// shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,	// filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP	// addressW
	);

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap
	(
		4,									// shaderRegister
		D3D12_FILTER_ANISOTROPIC,			// filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,	// addressW
		0.0f,								// mipLODBias
		8									// maxAnisotropy
	);

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp
	(
		5,									// shaderRegister
		D3D12_FILTER_ANISOTROPIC,			// filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	// addressW
		0.0f,								// mipLODBias
		8									// maxAnisotropy
	);

	return
	{
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp
	};
}