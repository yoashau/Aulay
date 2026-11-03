// Render the production icon without starting Bluetooth or changing OS settings.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wil/common.h>
#include <wil/result.h>
#include "../TrayIcon.hpp"
#include <iostream>
#include <stdexcept>

void Require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void CheckStrokeWeight()
{
	// This enlarged cross-section passes through both vertical stems and the
	// upper diagonals, away from their joins, for Fluent and MDL2 artwork.
	constexpr int size = 256, row = 77;
	auto image = AulayTrayIcon::Render(size, size, D2D1::ColorF(D2D1::ColorF::White));
	std::vector<BYTE> pixels(size * size * 4);
	winrt::check_hresult(image->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()));
	struct Run {
		int begin, end;
		float coverage;
	};
	std::vector<Run> runs;
	int begin = -1;
	float coverage = 0;
	for (int x = 0; x <= size; ++x) {
		auto alpha = x < size ? pixels[(row * size + x) * 4 + 3] : 0;
		if (alpha) {
			if (begin < 0)
				begin = x;
			coverage += alpha / 255.0f;
		} else if (begin >= 0) {
			runs.push_back({ begin, x, coverage });
			begin = -1;
			coverage = 0;
		}
	}
	Require(runs.size() == 4, "could not isolate glyph stroke cross-sections");
	// The widest gap separates the speaker's right stem from Bluetooth's stem.
	size_t gap = 0;
	for (size_t i = 1; i + 1 < runs.size(); ++i) {
		if (runs[i + 1].begin - runs[i].end > runs[gap + 1].begin - runs[gap].end)
			gap = i;
	}
	Require(gap == 1, "unexpected glyph placement in stroke cross-section");
	constexpr float stroke = size / 16.0f, tolerance = stroke * 0.05f;
	Require(std::abs(runs[1].coverage - stroke) < tolerance, "speaker stem lost native line weight");
	Require(std::abs(runs[2].coverage - runs[1].coverage) < tolerance, "Bluetooth stem is thinner than speaker stem");
	Require(std::abs(runs[3].coverage - runs[0].coverage) / std::sqrt(2.0f) < tolerance, "Bluetooth diagonal has a different line weight");
}

int main()
{
	try {
		winrt::init_apartment();
		int frames = 0;
		for (int size : { 16, 20, 24, 28, 32, 40, 48, 64 }) {
			for (auto color : { D2D1::ColorF(0.0f, 0.0f, 0.0f, 228.0f / 255.0f),
					 D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Yellow) }) {
				auto image = AulayTrayIcon::Render(size, size, color);
				UINT width = 0, height = 0;
				winrt::check_hresult(image->GetSize(&width, &height));
				Require(width == size && height == size, "wrong physical size");
				std::vector<BYTE> pixels(size * size * 4);
				winrt::check_hresult(image->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()));
				int covered = 0, antialiased = 0, left = size, right = -1, top = size, bottom = -1;
				BYTE maxAlpha = 0;
				for (int y = 0; y < size; ++y) {
					for (int x = 0; x < size; ++x) {
						auto pixel = &pixels[(y * size + x) * 4];
						auto alpha = pixel[3];
						Require(pixel[0] <= alpha && pixel[1] <= alpha && pixel[2] <= alpha, "invalid premultiplied alpha");
						if (!color.r)
							Require(!pixel[0] && !pixel[1] && !pixel[2], "black icon has a color fringe");
						else if (color.b)
							Require(pixel[0] == alpha && pixel[1] == alpha && pixel[2] == alpha, "white icon has a color fringe");
						if (!alpha)
							continue;
						++covered;
						antialiased += alpha < 220;
						maxAlpha = std::max(maxAlpha, alpha);
						left = std::min(left, x);
						right = std::max(right, x);
						top = std::min(top, y);
						bottom = std::max(bottom, y);
					}
				}
				Require(covered > size && covered < size * size / 2, "empty or solid icon");
				Require(antialiased > 0, "missing edge antialiasing");
				Require(top > 0 && bottom < size - 1, "vertical artwork clipped");
				Require(std::abs(left - (size - 1 - right)) <= 1, "horizontal artwork off center");
				Require(std::abs(top - (size - 1 - bottom)) <= 1, "vertical artwork off center");
				Require(maxAlpha <= std::round(color.a * 255), "foreground opacity lost");
				++frames;
			}
			wil::unique_hicon icon(AulayTrayIcon::Create(size, size));
			ICONINFO info {};
			winrt::check_bool(GetIconInfo(icon.get(), &info));
			wil::unique_hbitmap color(info.hbmColor), mask(info.hbmMask);
			BITMAP bitmap {};
			Require(GetObjectW(color.get(), sizeof(bitmap), &bitmap) != 0, "missing HICON color bitmap");
			Require(bitmap.bmWidth == size && bitmap.bmHeight == size && bitmap.bmBitsPixel == 32, "wrong HICON dimensions or depth");
		}
		CheckStrokeWeight();
		// Theme/display changes recreate the HICON; repeated updates must release GDI objects.
		DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
		for (int i = 0; i < 64; ++i) {
			wil::unique_hicon icon(AulayTrayIcon::Create(24, 24));
		}
		Require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before, "GDI object leak on refresh");
		std::cout << "TRAY_ICON_RESULT=PASS: " << frames << " frames; 100-400% DPI, light/dark/contrast colors, HICON depth and GDI lifetime\n";
		return 0;
	} catch (std::exception const& error) {
		std::cerr << "TRAY_ICON_RESULT=FAIL: " << error.what() << '\n';
	} catch (winrt::hresult_error const& error) {
		std::wcerr << L"TRAY_ICON_RESULT=FAIL: " << error.message().c_str() << '\n';
	}
	return 1;
}
