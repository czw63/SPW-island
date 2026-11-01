// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <cstring>
#include <string>

// Maps an FP16 scRGB desktop crop back to 8-bit sRGB on the GPU.
//
// Skia's Direct3D backend cannot wrap a shared R16G16B16A16_FLOAT resource as a render target:
// Surface.makeFromBackendRenderTarget returns null for it, so FP16 cannot cross the shared
// handoff at all. Converting here keeps every shared texture B8G8R8A8_UNORM, which the
// existing transport already proved on real machines, and the optics still receive encoded
// sRGB values.
//
// scRGB 1.0 is D65 white at 80 nits while Windows renders SDR content at the user's paper
// white, so dividing by paperWhite/80 reproduces every SDR pixel exactly. Only content above
// paper white rolls off, compressed on the brightest channel, so highlights desaturate
// towards white instead of shifting hue.
class HdrToneMap {
    winrt::com_ptr<ID3D11VertexShader> vertex;
    winrt::com_ptr<ID3D11PixelShader> pixel;
    winrt::com_ptr<ID3D11Buffer> parameters;
    winrt::com_ptr<ID3D11Texture2D> input;
    winrt::com_ptr<ID3D11ShaderResourceView> inputView;
    winrt::com_ptr<ID3D11Texture2D> output;
    winrt::com_ptr<ID3D11RenderTargetView> outputView;
    ID3D11Device* device = nullptr;
    UINT width = 0, height = 0;
    HRESULT failure = S_OK;

    static constexpr char const* kShader = R"(
        cbuffer Parameters : register(b0) {
            float paperWhiteScale;
            float ceiling;
            float2 unused;
        };
        Texture2D<float4> source : register(t0);

        // Full-screen triangle; no vertex buffer or input layout is needed.
        float4 vertexMain(uint id : SV_VertexID) : SV_Position {
            float2 uv = float2((id << 1) & 2, id & 2);
            return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
        }

        float srgbEncode(float value) {
            return value <= 0.0031308 ? value * 12.92 : 1.055 * pow(abs(value), 1.0 / 2.4) - 0.055;
        }

        float4 pixelMain(float4 position : SV_Position) : SV_Target {
            // Load is exact; a sampler would blend neighbouring texels into the rolloff.
            float4 raw = source.Load(int3(int2(position.xy), 0));
            // Negative scRGB coordinates are out-of-gamut colours for this display.
            float3 linear = max(raw.rgb / paperWhiteScale, 0.0);
            float peak = max(max(linear.r, linear.g), linear.b);
            float ratio = 1.0;
            if (peak > 1.0) {
                // Compress on the brightest channel so the rolloff joins C1 at 1.0 and the
                // highlight approaches white instead of clipping to a flat plateau.
                float headroom = ceiling - 1.0;
                ratio = (1.0 + headroom * (1.0 - 1.0 / (1.0 + (peak - 1.0) / headroom))) / peak;
            }
            return float4(srgbEncode(linear.r * ratio), srgbEncode(linear.g * ratio),
                srgbEncode(linear.b * ratio), 1.0);
        }
    )";

    static void check(HRESULT result, wchar_t const* operation) {
        if (FAILED(result)) throw winrt::hresult_error(result, operation);
    }

    static void compileStage(char const* entry, char const* model, ID3DBlob** code,
                             wchar_t const* operation) {
        winrt::com_ptr<ID3DBlob> errors;
        const auto status = D3DCompile(kShader, std::strlen(kShader), "IslandGlassToneMap", nullptr,
            nullptr, entry, model, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, errors.put());
        if (FAILED(status)) {
            // The compiler message is the only actionable detail when a driver rejects the
            // program, so it is promoted into the exception the caller records.
            std::wstring detail(L"shader compilation failed");
            if (errors) detail = winrt::to_hstring(
                static_cast<char const*>(errors->GetBufferPointer())).c_str();
            throw winrt::hresult_error(status, (std::wstring(operation) + L": " + detail).c_str());
        }
    }

    void compile() {
        winrt::com_ptr<ID3DBlob> code;
        // Compiling once per capture session keeps the DLL independent of one shader model.
        compileStage("vertexMain", "vs_4_0", code.put(), L"Tone map vertex shader");
        check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
            vertex.put()), L"Tone map CreateVertexShader");
        code = nullptr;
        compileStage("pixelMain", "ps_4_0", code.put(), L"Tone map pixel shader");
        check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
            pixel.put()), L"Tone map CreatePixelShader");

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 16;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, parameters.put()), L"Tone map constant buffer");
    }

    void resize(UINT nextWidth, UINT nextHeight) {
        input = nullptr; inputView = nullptr; output = nullptr; outputView = nullptr;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = nextWidth;
        description.Height = nextHeight;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&description, nullptr, input.put()), L"Tone map input texture");
        check(device->CreateShaderResourceView(input.get(), nullptr, inputView.put()),
            L"Tone map input view");

        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&description, nullptr, output.put()), L"Tone map output texture");
        check(device->CreateRenderTargetView(output.get(), nullptr, outputView.put()),
            L"Tone map output view");
        width = nextWidth; height = nextHeight;
    }

public:
    /** False once compilation failed; HDR capture then reports itself as unavailable. */
    bool ready() const { return static_cast<bool>(pixel); }
    HRESULT error() const { return failure; }

    void initialize(ID3D11Device* target) {
        if (pixel || FAILED(failure)) return;
        device = target;
        try { compile(); }
        catch (winrt::hresult_error const& error) {
            failure = error.code();
            vertex = nullptr; pixel = nullptr; parameters = nullptr;
        } catch (...) {
            // Anything else here still only costs HDR, so it must not reach the worker's
            // top-level handler and tear down the whole capture session.
            failure = E_FAIL;
            vertex = nullptr; pixel = nullptr; parameters = nullptr;
        }
    }

    /**
     * Converts one FP16 crop into the internal 8-bit texture and returns it, or null when the
     * tone map is unavailable. The result is only valid until the next call.
     */
    ID3D11Texture2D* convert(ID3D11DeviceContext* context, ID3D11Texture2D* from,
                             RECT crop, POINT monitorOrigin, float paperWhiteScale, float ceiling) {
        if (!pixel) return nullptr;
        const UINT nextWidth = crop.right - crop.left, nextHeight = crop.bottom - crop.top;
        if (!nextWidth || !nextHeight) return nullptr;
        if (nextWidth != width || nextHeight != height) resize(nextWidth, nextHeight);
        const D3D11_BOX box{ static_cast<UINT>(crop.left - monitorOrigin.x),
            static_cast<UINT>(crop.top - monitorOrigin.y), 0,
            static_cast<UINT>(crop.right - monitorOrigin.x),
            static_cast<UINT>(crop.bottom - monitorOrigin.y), 1 };
        context->CopySubresourceRegion(input.get(), 0, 0, 0, 0, from, 0, &box);

        const float values[4]{ paperWhiteScale, ceiling, 0.0f, 0.0f };
        context->UpdateSubresource(parameters.get(), 0, nullptr, values, 0, 0);
        ID3D11ShaderResourceView* const views[]{ inputView.get() };
        ID3D11Buffer* const buffers[]{ parameters.get() };
        ID3D11RenderTargetView* const targets[]{ outputView.get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(nextWidth),
            static_cast<float>(nextHeight), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.get(), nullptr, 0);
        context->PSSetShader(pixel.get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, views);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->Draw(3, 0);
        // This immediate context also issues the next frame's copy, and D3D11 rejects a copy
        // that overlaps a live render-target or shader-resource binding.
        ID3D11RenderTargetView* const noTargets[]{ nullptr };
        ID3D11ShaderResourceView* const noViews[]{ nullptr };
        context->OMSetRenderTargets(1, noTargets, nullptr);
        context->PSSetShaderResources(0, 1, noViews);
        return output.get();
    }

    void reset() {
        input = nullptr; inputView = nullptr; output = nullptr; outputView = nullptr;
        width = 0; height = 0;
    }
};
