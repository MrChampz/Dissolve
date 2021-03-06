#include "RenderTarget.h"

RenderTarget::RenderTarget(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
	m_d3dDevice = device;

	m_Width = width;
	m_Height = height;
	m_Format = format;

	BuildResource();
}

void RenderTarget::OnResize(UINT width, UINT height)
{
	if ((m_Width != width) || (m_Height != height))
	{
		m_Width = width;
		m_Height = height;

		BuildResource();

		// New resource, so we need new descriptors to that resource.
		BuildDescriptors();
	}
}

void RenderTarget::BuildDescriptors(
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv)
{
	// Save references to the descriptors.
	m_CpuSrv = hCpuSrv;
	m_GpuSrv = hGpuSrv;
	m_CpuRtv = hCpuRtv;

	BuildDescriptors();
}

ID3D12Resource* RenderTarget::GetResource()
{
	return m_OffscreenTex.Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE RenderTarget::GetSrv()
{
	return m_GpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE RenderTarget::GetRtv()
{
	return m_CpuRtv;
}

bool RenderTarget::GetMSAAState() const
{
	return m_msaaState;
}

void RenderTarget::SetMSAAState(bool enabled, UINT level, UINT quality)
{
	if (m_msaaState != enabled)
	{
		m_msaaState   = enabled;
		m_msaaLevel   = level;
		m_msaaQuality = quality;

		// Recreate the buffer with new multisample settings.
		BuildResource();

		// New resource, so we need new descriptors to that resource.
		BuildDescriptors();
	}
}

void RenderTarget::BuildDescriptors()
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = m_Format;
	srvDesc.ViewDimension = m_msaaState ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;

	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = m_Format;
	rtvDesc.ViewDimension = m_msaaState ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;

	m_d3dDevice->CreateShaderResourceView(m_OffscreenTex.Get(), &srvDesc, m_CpuSrv);
	m_d3dDevice->CreateRenderTargetView(m_OffscreenTex.Get(), &rtvDesc, m_CpuRtv);
}

void RenderTarget::BuildResource()
{
	D3D12_RESOURCE_DESC texDesc;
	ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = m_Width;
	texDesc.Height = m_Height;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = m_Format;
	texDesc.SampleDesc.Count = m_msaaState ? m_msaaLevel : 1;
	texDesc.SampleDesc.Quality = m_msaaState ? (m_msaaQuality - 1) : 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = m_Format;
	memcpy(clearValue.Color, Colors::Black, sizeof(float) * 4);

	ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&clearValue,
		IID_PPV_ARGS(&m_OffscreenTex)));
}
