#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <wil/resource.h>
#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace AulayTrayIcon {

// Keep the speaker/Bluetooth meaning, but use the installed Windows artwork.
// Fluent is supplied by Windows 11; MDL2 supplies the same glyphs on Windows 10.
constexpr UINT32 Speaker = 0xE992;
constexpr UINT32 Bluetooth = 0xE702;
constexpr D2D1_COLOR_F LightForeground { 0.0f, 0.0f, 0.0f, 228.0f / 255.0f };

winrt::com_ptr<IDWriteFontFace> FontFace(IDWriteFactory* factory)
{
	winrt::com_ptr<IDWriteFontCollection> collection;
	winrt::check_hresult(factory->GetSystemFontCollection(collection.put()));
	for (auto name : { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" }) {
		UINT32 index = 0;
		BOOL exists = FALSE;
		winrt::check_hresult(collection->FindFamilyName(name, &index, &exists));
		if (!exists)
			continue;
		winrt::com_ptr<IDWriteFontFamily> family;
		winrt::com_ptr<IDWriteFont> font;
		winrt::com_ptr<IDWriteFontFace> face;
		winrt::check_hresult(collection->GetFontFamily(index, family.put()));
		winrt::check_hresult(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.put()));
		winrt::check_hresult(font->CreateFontFace(face.put()));
		UINT32 codes[] = { Speaker, Bluetooth };
		UINT16 glyphs[2] {};
		winrt::check_hresult(face->GetGlyphIndices(codes, 2, glyphs));
		if (glyphs[0] && glyphs[1])
			return face;
	}
	winrt::throw_hresult(DWRITE_E_NOFONT);
}

D2D1_COLOR_F Foreground()
{
	HIGHCONTRASTW contrast { sizeof(contrast) };
	if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
		auto color = GetSysColor(COLOR_WINDOWTEXT);
		return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
	}
	DWORD light = 1;
	DWORD size = sizeof(light);
	RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
	// Windows' primary icon foreground: translucent black on a light taskbar,
	// opaque white on a dark taskbar. Alpha lets the taskbar tint show through.
	return light ? LightForeground : D2D1::ColorF(D2D1::ColorF::White);
}

winrt::com_ptr<IWICBitmap> Render(int width, int height, D2D1_COLOR_F color)
{
	winrt::com_ptr<IDWriteFactory> write;
	winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown**>(write.put())));
	auto face = FontFace(write.get());
	winrt::com_ptr<ID2D1Factory> draw;
	winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, draw.put()));
	auto imaging = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
	winrt::com_ptr<IWICBitmap> bitmap;
	winrt::check_hresult(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put()));
	winrt::com_ptr<ID2D1RenderTarget> target;
	auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
	winrt::check_hresult(draw->CreateWicBitmapRenderTarget(bitmap.get(), properties, target.put()));
	target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	winrt::com_ptr<ID2D1SolidColorBrush> brush;
	winrt::check_hresult(target->CreateSolidColorBrush(color, brush.put()));

	float scale = static_cast<float>((std::min)(width, height)) / 16.0f;
	UINT32 codes[] = { Speaker, Bluetooth };
	UINT16 indices[2] {};
	winrt::check_hresult(face->GetGlyphIndices(codes, 2, indices));
	float sizes[] = { 16.0f * scale, 12.0f * scale };
	winrt::com_ptr<ID2D1PathGeometry> outlines[2];
	D2D1_RECT_F bounds[2] {};
	for (int i = 0; i < 2; ++i) {
		auto& outline = outlines[i];
		winrt::com_ptr<ID2D1GeometrySink> sink;
		winrt::check_hresult(draw->CreatePathGeometry(outline.put()));
		winrt::check_hresult(outline->Open(sink.put()));
		winrt::check_hresult(face->GetGlyphRunOutline(sizes[i], &indices[i], nullptr, nullptr, 1, FALSE, FALSE, sink.get()));
		winrt::check_hresult(sink->Close());
		if (i == 1) {
			// Both native glyphs have a 1/16-em stem. At the 16-DIP icon size,
			// Bluetooth's 12-DIP em size must not shrink its stroke to 0.75 DIP.
			// Union the added boundary before filling: painting twice would
			// darken overlapping translucent edges, especially at the crossing.
			winrt::com_ptr<ID2D1PathGeometry> stroke, corrected;
			winrt::com_ptr<ID2D1GeometrySink> strokeSink, unionSink;
			winrt::check_hresult(draw->CreatePathGeometry(stroke.put()));
			winrt::check_hresult(stroke->Open(strokeSink.put()));
			winrt::check_hresult(outline->Widen((sizes[0] - sizes[1]) / 16.0f, nullptr, nullptr, 0.01f * scale, strokeSink.get()));
			winrt::check_hresult(strokeSink->Close());
			winrt::check_hresult(draw->CreatePathGeometry(corrected.put()));
			winrt::check_hresult(corrected->Open(unionSink.put()));
			winrt::check_hresult(outline->CombineWithGeometry(stroke.get(), D2D1_COMBINE_MODE_UNION, nullptr, 0.01f * scale, unionSink.get()));
			winrt::check_hresult(unionSink->Close());
			outline = std::move(corrected);
		}
		winrt::check_hresult(outline->GetBounds(nullptr, &bounds[i]));
	}
	float speakerWidth = bounds[0].right - bounds[0].left;
	float inkWidth = speakerWidth + 1.25f * scale + bounds[1].right - bounds[1].left;
	float inkHeight = (std::max)(bounds[0].bottom - bounds[0].top, bounds[1].bottom - bounds[1].top);
	float x = std::round((width - inkWidth) / 2.0f);
	float y = std::round((height - inkHeight) / 2.0f);
	target->BeginDraw();
	target->Clear(D2D1::ColorF(0u, 0.0f));
	for (int i = 0; i < 2; ++i) {
		auto origin = D2D1::Point2F(x - bounds[i].left,
			y + (inkHeight - bounds[i].bottom + bounds[i].top) / 2.0f - bounds[i].top);
		target->SetTransform(D2D1::Matrix3x2F::Translation(origin.x, origin.y));
		target->FillGeometry(outlines[i].get(), brush.get());
		x += speakerWidth + 1.25f * scale;
	}
	winrt::check_hresult(target->EndDraw());
	return bitmap;
}

HICON Create(int width, int height)
{
	auto image = Render(width, height, Foreground());
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* pixels = nullptr;
	wil::unique_hbitmap color(CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
	winrt::check_bool(static_cast<bool>(color));
	winrt::check_hresult(image->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels)));
	// The alpha channel supplies coverage on modern shells. Also provide a
	// real AND mask for icon consumers that fall back to the legacy mask.
	int stride = ((width + 15) / 16) * 2;
	std::vector<BYTE> maskBits(stride * height, 0);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (!static_cast<BYTE*>(pixels)[(y * width + x) * 4 + 3])
				maskBits[y * stride + x / 8] |= 0x80 >> (x % 8);
		}
	}
	wil::unique_hbitmap mask(CreateBitmap(width, height, 1, 1, maskBits.data()));
	winrt::check_bool(static_cast<bool>(mask));
	ICONINFO iconInfo {};
	iconInfo.fIcon = TRUE;
	iconInfo.hbmColor = color.get();
	iconInfo.hbmMask = mask.get();
	auto icon = CreateIconIndirect(&iconInfo);
	winrt::check_bool(icon != nullptr);
	return icon;
}

}
