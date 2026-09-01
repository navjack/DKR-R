#include "runtime_ui.hpp"

#include "game_registration.hpp"
#include "generated/jumpman_font.h"
#include "generated/racing_banana_font.h"
#include "magic_code_policy.hpp"
#include "modern_camera_policy.hpp"
#include "runtime_enhancements.hpp"
#include "runtime_audio_controls.hpp"
#include "runtime_crt_overlay.hpp"
#include "runtime_input.hpp"
#include "runtime_magic_codes.hpp"
#include "runtime_platform.hpp"
#include "runtime_telemetry.hpp"
#include "runtime_texture_packs.hpp"
#include "save_manager.hpp"
#include "virtual_pak.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Unknwn.h>
#include <oaidl.h>
#endif

#include "gui/rt64_inspector.h"
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2_custom.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"
#include "nfd.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using ultramodern::renderer::Antialiasing;
using ultramodern::renderer::AspectRatio;
using ultramodern::renderer::GraphicsApi;
using ultramodern::renderer::GraphicsConfig;
using ultramodern::renderer::HighPrecisionFramebuffer;
using ultramodern::renderer::HUDRatioMode;
using ultramodern::renderer::Resolution;
using ultramodern::renderer::RefreshRate;
using ultramodern::renderer::WindowMode;

std::filesystem::path g_config_directory;
std::atomic<bool> g_overlay_visible{false};
// This is a one-shot navigation request, not a redraw flag. The overlay is
// drawn every presented frame while it is visible. Setting this for ordinary
// SDL events forces ImGui back onto the selected sidebar button after every
// mouse click or gamepad direction, which makes the content panel impossible
// to operate once the RT64 inspector owns the in-game UI.
std::atomic<bool> g_overlay_focus_requested{false};
std::atomic<dkr::runtime::ui::LifecycleRequest> g_lifecycle_request{
    dkr::runtime::ui::LifecycleRequest::None};
std::mutex g_inspector_guard;
RT64::Inspector* g_inspector = nullptr;
std::atomic<int> g_overlay_page{0};
ImFont* g_font_heading = nullptr;
ImFont* g_font_title = nullptr;
ImFont* g_font_controls = nullptr;
ImFont* g_font_fps = nullptr;
int g_brand_logo_rect = -1;
RefreshRate g_modern_refresh_mode = RefreshRate::Display;
int g_modern_refresh_target = 60;
Resolution g_modern_resolution = Resolution::Auto;
AspectRatio g_modern_aspect = AspectRatio::Expand;
Antialiasing g_modern_antialiasing = Antialiasing::None;
HighPrecisionFramebuffer g_modern_high_precision_fb = HighPrecisionFramebuffer::On;
GraphicsApi g_modern_graphics_api = GraphicsApi::Auto;
int g_modern_downsample = 1;
bool g_fps_overlay_enabled = false;
int g_fps_overlay_position = 1;
int g_fps_overlay_detail = 1;
bool g_fps_overlay_single_row = false;
bool g_fps_custom_frame_time = true;
bool g_fps_custom_simulation = true;
bool g_fps_custom_graphics = false;
bool g_fps_custom_vi = false;
bool g_fps_custom_interpolation = false;
bool g_fps_custom_audio = false;
bool g_fps_custom_target = true;
bool g_fps_custom_resolution = false;
ImVec4 g_fps_fill_colour{0.96F, 0.18F, 0.10F, 1.0F};
ImVec4 g_fps_outline_colour{1.0F, 0.58F, 0.04F, 1.0F};
int g_fps_font_size = 24;
bool g_crt_enabled = false;
int g_crt_filter_index = 0;
int g_crt_scale_mode = 0;
float g_crt_strength = 0.35F;
std::string g_crt_status;
std::string g_texture_pack_status;
bool g_show_hidden_texture_packs = false;
std::string g_texture_pack_remove_id;
std::string g_texture_pack_remove_name;

constexpr int kPagePlay = 0;
constexpr int kPageGraphics = 1;
constexpr int kPageSound = 2;
constexpr int kPageControls = 3;
constexpr int kPageSaveManager = 4;
constexpr int kPageOnlineMp = 5;
constexpr int kPageModsHacks = 6;
constexpr int kPageAbout = 7;
constexpr int kMenuPageCount = 8;

struct CrtFilterEntry {
    std::string label;
    std::filesystem::path path;
    bool built_in = false;
};

std::vector<CrtFilterEntry> g_crt_filters;

#ifndef DKR_RELEASE_VERSION
#define DKR_RELEASE_VERSION "development"
#endif
enum class CaptureDevice { None, Keyboard, Controller };
CaptureDevice g_capture_device = CaptureDevice::None;
int g_capture_action = -1;
bool g_capture_popup_pending = false;
bool g_capture_finished = false;
constexpr int kShortcutCaptureAction = -2;
std::array<int, 2> g_shortcut_capture_sources{
    dkr::runtime::input::kUnbound, dkr::runtime::input::kUnbound};
int g_shortcut_capture_count = 0;
std::chrono::steady_clock::time_point g_shortcut_capture_deadline{};
std::string g_save_manager_status;
std::optional<dkr::runtime::saves::codec::SaveImage> g_save_builder_image;
int g_save_builder_slot = 0;

bool HandleInputCaptureEvent(SDL_Event* event);

struct BrowserEntry {
    std::filesystem::path path;
    bool directory = false;
};

struct RomBrowserState {
    bool open = false;
    bool close_requested = false;
    bool focus_first_entry = false;
    std::filesystem::path directory;
    std::vector<BrowserEntry> entries;
    std::string message;
};

RomBrowserState g_rom_browser;

constexpr ImVec4 kBackground{0.02F, 0.20F, 0.45F, 1.0F};
constexpr ImVec4 kPanel{0.025F, 0.20F, 0.43F, 1.0F};
constexpr ImVec4 kPanelSoft{0.05F, 0.34F, 0.62F, 1.0F};
constexpr ImVec4 kText{1.0F, 0.965F, 0.855F, 1.0F};
constexpr ImVec4 kMuted{0.69F, 0.79F, 0.80F, 1.0F};
constexpr ImVec4 kAccent{0.10F, 0.76F, 0.64F, 1.0F};
constexpr ImVec4 kWarm{1.0F, 0.67F, 0.08F, 1.0F};
constexpr ImVec4 kRaceRed{0.91F, 0.18F, 0.13F, 1.0F};
constexpr ImVec4 kRaceBlue{0.04F, 0.43F, 0.63F, 1.0F};
constexpr ImVec4 kCream{1.0F, 0.94F, 0.76F, 1.0F};

std::string PathUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::filesystem::path RuntimeAssetPath(const std::filesystem::path& relative) {
    if (char* base = SDL_GetBasePath(); base != nullptr) {
        const std::filesystem::path candidate =
            std::filesystem::path(base) / relative;
        SDL_free(base);
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    const std::filesystem::path candidate =
        std::filesystem::current_path() / relative;
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error)
        ? candidate : std::filesystem::path{};
}

std::filesystem::path BrandLogoPath() {
    return RuntimeAssetPath("assets/ui/Icons/DKR-R8.bmp");
}

void RefreshCrtFilters() {
    static constexpr std::array<std::pair<const char*, const char*>, 6>
        built_ins{{
            {"Soft scanlines", "scanlines-soft.png"},
            {"Shadow mask", "shadow-mask.png"},
            {"Aperture grille", "aperture-grille.png"},
            {"Perfect CRT 240p Bright", "Perfect_CRT-240p-BRT.png"},
            {"Perfect CRT 240p", "Perfect_CRT-240p.png"},
            {"Perfect CRT", "Perfect_CRT.png"},
        }};
    g_crt_filters.clear();
    for (const auto& [label, file] : built_ins) {
        g_crt_filters.push_back(
            {label, RuntimeAssetPath(std::filesystem::path("assets/filters") / file),
             true});
    }

    const std::filesystem::path custom_directory =
        g_config_directory / "filters";
    std::error_code error;
    std::filesystem::create_directories(custom_directory, error);
    std::vector<std::filesystem::path> custom_paths;
    if (!error) {
        for (const auto& entry :
             std::filesystem::directory_iterator(custom_directory, error)) {
            if (error) break;
            if (!entry.is_regular_file(error)) continue;
            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            if (extension == ".png") custom_paths.push_back(entry.path());
        }
    }
    std::sort(custom_paths.begin(), custom_paths.end());
    for (const auto& path : custom_paths) {
        g_crt_filters.push_back({path.stem().string() + " (Custom)", path, false});
    }
    if (g_crt_filters.empty()) {
        g_crt_filter_index = 0;
    } else {
        g_crt_filter_index = std::clamp(
            g_crt_filter_index, 0, static_cast<int>(g_crt_filters.size()) - 1);
    }
}

bool CrtFilterGetter(void* data, int index, const char** output) {
    const auto* filters = static_cast<const std::vector<CrtFilterEntry>*>(data);
    if (filters == nullptr || index < 0 ||
        index >= static_cast<int>(filters->size())) {
        return false;
    }
    *output = (*filters)[static_cast<std::size_t>(index)].label.c_str();
    return true;
}

void LoadBrandLogoIntoAtlas() {
    g_brand_logo_rect = -1;
    const std::filesystem::path path = BrandLogoPath();
    if (path.empty()) {
        std::fprintf(stderr,
                     "[boot][ui] DKR-R8 launcher logo was not found beside the runtime\n");
        return;
    }
    SDL_Surface* source = SDL_LoadBMP(PathUtf8(path).c_str());
    if (source == nullptr) {
        std::fprintf(stderr, "[boot][ui] launcher logo could not be loaded: %s\n",
                     SDL_GetError());
        return;
    }
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(
        source, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(source);
    if (rgba == nullptr || rgba->w <= 0 || rgba->h <= 0) {
        std::fprintf(stderr, "[boot][ui] launcher logo conversion failed: %s\n",
                     SDL_GetError());
        if (rgba != nullptr) SDL_FreeSurface(rgba);
        return;
    }

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const int rect_index = atlas->AddCustomRectRegular(rgba->w, rgba->h);
    unsigned char* pixels = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    ImFontAtlasCustomRect* rect = atlas->GetCustomRectByIndex(rect_index);
    if (pixels == nullptr || rect == nullptr || !rect->IsPacked() ||
        rect->X + rect->Width > atlas_width ||
        rect->Y + rect->Height > atlas_height) {
        std::fprintf(stderr, "[boot][ui] launcher logo would not fit the font atlas\n");
        SDL_FreeSurface(rgba);
        return;
    }
    for (int row = 0; row < rgba->h; ++row) {
        const auto* source_row = static_cast<const unsigned char*>(rgba->pixels) +
            static_cast<std::size_t>(row) * rgba->pitch;
        auto* destination_row = pixels +
            (static_cast<std::size_t>(rect->Y + row) * atlas_width + rect->X) * 4U;
        std::memcpy(destination_row, source_row,
                    static_cast<std::size_t>(rgba->w) * 4U);
    }
    atlas->TexPixelsUseColors = true;
    g_brand_logo_rect = rect_index;
    SDL_FreeSurface(rgba);
}

std::filesystem::path SettingsPath() {
    return g_config_directory / "dkr-port-settings.ini";
}

std::filesystem::path LastRomPath() {
    return g_config_directory / "last-rom.txt";
}

bool ReplaceSettingsFile(const std::filesystem::path& temporary,
                         const std::filesystem::path& destination) {
#if defined(_WIN32)
    return MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 18.0F;
    style.FrameRounding = 12.0F;
    style.PopupRounding = 16.0F;
    style.ScrollbarRounding = 12.0F;
    style.GrabRounding = 12.0F;
    style.WindowPadding = {0.0F, 0.0F};
    style.FramePadding = {16.0F, 11.0F};
    style.ItemSpacing = {12.0F, 13.0F};
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 2.0F;
    style.FrameBorderSize = 1.0F;
    style.Colors[ImGuiCol_WindowBg] = kBackground;
    style.Colors[ImGuiCol_ChildBg] = kPanel;
    style.Colors[ImGuiCol_Border] = {0.16F, 0.42F, 0.44F, 1.0F};
    style.Colors[ImGuiCol_Text] = kText;
    style.Colors[ImGuiCol_TextDisabled] = kMuted;
    style.Colors[ImGuiCol_FrameBg] = kPanelSoft;
    style.Colors[ImGuiCol_FrameBgHovered] = {0.09F, 0.28F, 0.33F, 1.0F};
    style.Colors[ImGuiCol_FrameBgActive] = {0.10F, 0.36F, 0.38F, 1.0F};
    style.Colors[ImGuiCol_Button] = kRaceBlue;
    style.Colors[ImGuiCol_ButtonHovered] = {0.98F, 0.43F, 0.08F, 1.0F};
    style.Colors[ImGuiCol_ButtonActive] = kRaceRed;
    style.Colors[ImGuiCol_CheckMark] = kAccent;
    style.Colors[ImGuiCol_SliderGrab] = kWarm;
    style.Colors[ImGuiCol_SliderGrabActive] = kRaceRed;
    style.Colors[ImGuiCol_Header] = {0.04F, 0.38F, 0.43F, 1.0F};
    style.Colors[ImGuiCol_HeaderHovered] = {0.98F, 0.43F, 0.08F, 1.0F};
    style.Colors[ImGuiCol_HeaderActive] = kRaceRed;
    style.Colors[ImGuiCol_Separator] = {0.95F, 0.55F, 0.08F, 0.75F};
    // Controller navigation must read as a deliberate selection, even over
    // the blue half of the racing backdrop. Warm yellow is shared with the
    // start lights and remains distinct from every card and button colour.
    style.Colors[ImGuiCol_NavHighlight] = {1.0F, 0.82F, 0.12F, 1.0F};
}

void LoadLauncherFonts() {
    ImGuiIO& io = ImGui::GetIO();
    static constexpr ImWchar kLauncherGlyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin and Latin-1 Supplement.
        0,
    };

    const auto merge_fallback = [&](float size) {
        ImFontConfig fallback_config{};
        fallback_config.MergeMode = true;
        fallback_config.PixelSnapH = true;
        fallback_config.OversampleH = 1;
        fallback_config.OversampleV = 1;
        fallback_config.SizePixels = size;
        fallback_config.RasterizerMultiply = 1.35F;
        fallback_config.GlyphRanges = kLauncherGlyphRanges;
        io.Fonts->AddFontDefault(&fallback_config);
    };
    const auto add_racing_font = [&](float size) -> ImFont* {
        ImFontConfig config{};
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        config.OversampleV = 2;
        config.GlyphRanges = kLauncherGlyphRanges;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            const_cast<char*>(dkr_racing_banana_font),
            static_cast<int>(dkr_racing_banana_font_size), size, &config,
            kLauncherGlyphRanges);
        if (font != nullptr) {
            merge_fallback(size);
        }
        return font;
    };

    // Numeric and selectable controls use one coherent sans face. Racing
    // Banana deliberately remains the launcher's body font, but its missing
    // digits otherwise force ImGui to mix fallback glyphs of a different
    // apparent size inside sliders and combo previews.
    ImFontConfig control_config{};
    control_config.SizePixels = 21.0F;
    control_config.OversampleH = 2;
    control_config.OversampleV = 2;
    control_config.PixelSnapH = true;
    control_config.RasterizerMultiply = 1.20F;
    control_config.GlyphRanges = kLauncherGlyphRanges;
    g_font_controls = io.Fonts->AddFontDefault(&control_config);

    ImFont* body_font = add_racing_font(19.0F);
    if (body_font == nullptr) {
        ImFontConfig fallback_config{};
        fallback_config.SizePixels = 19.0F;
        body_font = io.Fonts->AddFontDefault(&fallback_config);
        std::fprintf(stderr,
                     "[boot][ui] Racing Banana body font could not be loaded; using fallback\n");
    }
    io.FontDefault = body_font;
    if (g_font_controls == nullptr) {
        g_font_controls = body_font;
    }

    const auto add_jumpman_font = [&](float size) -> ImFont* {
        ImFontConfig config{};
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        config.OversampleV = 2;
        config.GlyphRanges = kLauncherGlyphRanges;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(dkr_jumpman_font),
            static_cast<int>(dkr_jumpman_font_size), size, &config,
            kLauncherGlyphRanges);
        if (font != nullptr) {
            merge_fallback(size);
        }
        return font;
    };
    g_font_heading = add_jumpman_font(52.0F);
    g_font_title = add_jumpman_font(68.0F);
    g_font_fps = add_jumpman_font(24.0F);
    if (g_font_heading == nullptr || g_font_title == nullptr) {
        std::fprintf(stderr,
                     "[boot][ui] Jumpman heading font could not be loaded; using body font\n");
        g_font_heading = body_font;
        g_font_title = body_font;
    }
    if (g_font_fps == nullptr) {
        g_font_fps = g_font_controls != nullptr ? g_font_controls : body_font;
    }
    LoadBrandLogoIntoAtlas();
}

class ControlFontScope {
public:
    explicit ControlFontScope(bool pad_popup = false)
        : font_pushed_(g_font_controls != nullptr),
          padding_pushed_(pad_popup) {
        if (font_pushed_) {
            ImGui::PushFont(g_font_controls);
        }
        if (padding_pushed_) {
            // WindowPadding is sampled when BeginCombo creates its popup; it
            // does not disturb the already-open launcher card. This leaves a
            // comfortable cap above the first row and below the last row.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(16.0F, 12.0F));
        }
    }

    ~ControlFontScope() {
        if (padding_pushed_) {
            ImGui::PopStyleVar();
        }
        if (font_pushed_) {
            ImGui::PopFont();
        }
    }

    ControlFontScope(const ControlFontScope&) = delete;
    ControlFontScope& operator=(const ControlFontScope&) = delete;

private:
    bool font_pushed_ = false;
    bool padding_pushed_ = false;
};

bool ControlCombo(const char* label, int* current_item,
                  const char* items_separated_by_zeros,
                  int popup_max_height_in_items = -1) {
    const ControlFontScope scope(true);
    return ImGui::Combo(label, current_item, items_separated_by_zeros,
                        popup_max_height_in_items);
}

bool ControlCombo(const char* label, int* current_item,
                  bool (*items_getter)(void*, int, const char**), void* data,
                  int items_count, int popup_max_height_in_items = -1) {
    const ControlFontScope scope(true);
    return ImGui::Combo(label, current_item, items_getter, data, items_count,
                        popup_max_height_in_items);
}

bool ControlSliderInt(const char* label, int* value, int minimum, int maximum,
                      const char* format = "%d",
                      ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    const ControlFontScope scope;
    return ImGui::SliderInt(label, value, minimum, maximum, format, flags);
}

bool ControlSliderFloat(const char* label, float* value, float minimum,
                        float maximum, const char* format = "%.3f",
                        ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    const ControlFontScope scope;
    return ImGui::SliderFloat(label, value, minimum, maximum, format, flags);
}

bool DrawColourPickerButton(const char* label, const char* popup_id,
                            ImVec4& colour, float width) {
    bool changed = false;
    ImGui::TextUnformatted(label);
    const ControlFontScope scope;
    if (ImGui::ColorButton(popup_id, colour,
                           ImGuiColorEditFlags_AlphaPreviewHalf,
                           ImVec2(width, 34.0F))) {
        ImGui::OpenPopup(popup_id);
    }
    if (ImGui::BeginPopup(popup_id)) {
        ImGui::TextUnformatted(label);
        if (ImGui::ColorPicker4(
                "##colour-picker", &colour.x,
                ImGuiColorEditFlags_AlphaBar |
                    ImGuiColorEditFlags_AlphaPreviewHalf |
                    ImGuiColorEditFlags_NoInputs |
                    ImGuiColorEditFlags_NoSidePreview)) {
            changed = true;
        }
        ImGui::EndPopup();
    }
    return changed;
}

void PushHeadingFont(bool title = false) {
    ImFont* font = title ? g_font_title : g_font_heading;
    if (font != nullptr) {
        ImGui::PushFont(font);
    }
}

void PopHeadingFont(bool title = false) {
    if ((title ? g_font_title : g_font_heading) != nullptr) {
        ImGui::PopFont();
    }
}

void DrawPageHeading(const char* text, bool title = false) {
    ImFont* font = title ? g_font_title : g_font_heading;
    if (font == nullptr || text == nullptr) {
        ImGui::TextUnformatted(text != nullptr ? text : "");
        return;
    }
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float size = font->FontSize;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0F, text);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    constexpr float outline = 2.0F;
    const ImU32 orange = ImGui::ColorConvertFloat4ToU32(kWarm);
    const ImU32 red = ImGui::ColorConvertFloat4ToU32(kRaceRed);
    draw->AddText(font, size, {position.x - outline, position.y}, orange, text);
    draw->AddText(font, size, {position.x + outline, position.y}, orange, text);
    draw->AddText(font, size, {position.x, position.y - outline}, orange, text);
    draw->AddText(font, size, {position.x, position.y + outline}, orange, text);
    draw->AddText(font, size, position, red, text);
    ImGui::Dummy({extent.x, extent.y + 2.0F});
}

void RememberModernGraphics(const GraphicsConfig& config) {
    g_modern_resolution = config.res_option;
    g_modern_aspect = config.ar_option == AspectRatio::Original
        ? AspectRatio::Original
        : AspectRatio::Expand;
    g_modern_antialiasing = config.msaa_option;
    g_modern_high_precision_fb = config.hpfb_option;
    g_modern_graphics_api = config.api_option;
    g_modern_downsample = std::clamp(config.ds_option, 1, 8);
    if (config.rr_option == RefreshRate::Display ||
        config.rr_option == RefreshRate::Manual) {
        g_modern_refresh_mode = config.rr_option;
        g_modern_refresh_target =
            dkr::runtime::enhancements::clamp_presentation_rate(
                config.rr_manual_value);
    }
}

void ApplyProfileGraphics(GraphicsConfig& config,
                          dkr::runtime::enhancements::PresentationProfile profile) {
    if (profile == dkr::runtime::enhancements::PresentationProfile::Modern) {
        config.res_option = g_modern_resolution;
        config.ar_option = g_modern_aspect;
        config.msaa_option = g_modern_antialiasing;
        config.hpfb_option = g_modern_high_precision_fb;
        config.api_option = g_modern_graphics_api;
        config.hr_option = HUDRatioMode::Original;
        config.ds_option = g_modern_downsample;
        config.rr_option = g_modern_refresh_mode;
        config.rr_manual_value = g_modern_refresh_target;
        return;
    }

    // Accurate is enforced again inside the renderer. These launcher values
    // make the effective state explicit and ensure stale Modern preferences do
    // not leak into a pre-launch configuration snapshot.
    config.res_option = Resolution::Auto;
    config.ar_option = AspectRatio::Original;
    config.msaa_option = Antialiasing::None;
    config.hpfb_option = HighPrecisionFramebuffer::Auto;
    config.api_option = GraphicsApi::Auto;
    config.hr_option = HUDRatioMode::Original;
    config.ds_option = 1;
    config.rr_option = RefreshRate::Original;
    config.rr_manual_value = 30;
}

void SaveSettings() {
    std::error_code error;
    std::filesystem::create_directories(g_config_directory, error);
    if (error) {
        std::fprintf(stderr, "[boot][settings] failed to create settings directory: %s\n",
                     error.message().c_str());
        return;
    }
    const auto& config = ultramodern::renderer::get_graphics_config();
    const std::filesystem::path settings_path = SettingsPath();
    const std::filesystem::path temporary_path = settings_path.string() + ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::trunc);
        if (!output) {
            std::fprintf(stderr, "[boot][settings] failed to open temporary settings file\n");
            return;
        }
        output << "settings_version="
               << dkr::runtime::enhancements::kCurrentSettingsVersion << '\n';
        output << "presentation_profile="
               << static_cast<int>(dkr::runtime::enhancements::presentation_profile()) << '\n';
        output << "window_mode=" << static_cast<int>(config.wm_option) << '\n';
        output << "resolution=" << static_cast<int>(config.res_option) << '\n';
        output << "aspect=" << static_cast<int>(config.ar_option) << '\n';
        output << "antialiasing=" << static_cast<int>(config.msaa_option) << '\n';
        output << "high_precision_fb=" << static_cast<int>(config.hpfb_option) << '\n';
        output << "graphics_api=" << static_cast<int>(config.api_option) << '\n';
        output << "refresh_rate=" << static_cast<int>(config.rr_option) << '\n';
        output << "refresh_rate_target="
               << dkr::runtime::enhancements::clamp_presentation_rate(
                      config.rr_manual_value) << '\n';
        output << "modern_refresh_rate=" << static_cast<int>(g_modern_refresh_mode) << '\n';
        output << "modern_refresh_target="
               << dkr::runtime::enhancements::clamp_presentation_rate(
                      g_modern_refresh_target) << '\n';
        output << "modern_resolution=" << static_cast<int>(g_modern_resolution) << '\n';
        output << "modern_aspect=" << static_cast<int>(g_modern_aspect) << '\n';
        output << "modern_antialiasing="
               << static_cast<int>(g_modern_antialiasing) << '\n';
        output << "modern_high_precision_fb="
               << static_cast<int>(g_modern_high_precision_fb) << '\n';
        output << "modern_graphics_api="
               << static_cast<int>(g_modern_graphics_api) << '\n';
        output << "modern_downsample=" << g_modern_downsample << '\n';
        output << "modern_anisotropy="
               << dkr::runtime::enhancements::anisotropy_level() << '\n';
        output << "fps_overlay_enabled=" << (g_fps_overlay_enabled ? 1 : 0) << '\n';
        output << "fps_overlay_position=" << g_fps_overlay_position << '\n';
        output << "fps_overlay_detail=" << g_fps_overlay_detail << '\n';
        output << "fps_overlay_single_row=" << (g_fps_overlay_single_row ? 1 : 0) << '\n';
        output << "fps_custom_frame_time=" << (g_fps_custom_frame_time ? 1 : 0) << '\n';
        output << "fps_custom_simulation=" << (g_fps_custom_simulation ? 1 : 0) << '\n';
        output << "fps_custom_graphics=" << (g_fps_custom_graphics ? 1 : 0) << '\n';
        output << "fps_custom_vi=" << (g_fps_custom_vi ? 1 : 0) << '\n';
        output << "fps_custom_interpolation=" << (g_fps_custom_interpolation ? 1 : 0) << '\n';
        output << "fps_custom_audio=" << (g_fps_custom_audio ? 1 : 0) << '\n';
        output << "fps_custom_target=" << (g_fps_custom_target ? 1 : 0) << '\n';
        output << "fps_custom_resolution=" << (g_fps_custom_resolution ? 1 : 0) << '\n';
        output << "fps_font_size=" << g_fps_font_size << '\n';
        output << "fps_fill_r=" << static_cast<int>(g_fps_fill_colour.x * 255.0F) << '\n';
        output << "fps_fill_g=" << static_cast<int>(g_fps_fill_colour.y * 255.0F) << '\n';
        output << "fps_fill_b=" << static_cast<int>(g_fps_fill_colour.z * 255.0F) << '\n';
        output << "fps_fill_a=" << static_cast<int>(g_fps_fill_colour.w * 255.0F) << '\n';
        output << "fps_outline_r=" << static_cast<int>(g_fps_outline_colour.x * 255.0F) << '\n';
        output << "fps_outline_g=" << static_cast<int>(g_fps_outline_colour.y * 255.0F) << '\n';
        output << "fps_outline_b=" << static_cast<int>(g_fps_outline_colour.z * 255.0F) << '\n';
        output << "fps_outline_a=" << static_cast<int>(g_fps_outline_colour.w * 255.0F) << '\n';
        output << "crt_enabled=" << (g_crt_enabled ? 1 : 0) << '\n';
        output << "crt_filter_index=" << g_crt_filter_index << '\n';
        output << "crt_scale_mode=" << g_crt_scale_mode << '\n';
        output << "crt_strength="
               << static_cast<int>(std::clamp(g_crt_strength, 0.0F, 1.0F) *
                                   1000.0F)
               << '\n';
        output << "master_volume=" << dkr::runtime::platform::master_volume() << '\n';
        output << "music_volume=" << dkr::runtime::audio::music_volume() << '\n';
        output << "sound_effects_volume=" << dkr::runtime::audio::sound_effects_volume() << '\n';
        output << "vehicle_volume=" << dkr::runtime::audio::vehicle_volume() << '\n';
        output << "nature_volume=" << dkr::runtime::audio::nature_volume() << '\n';
        output << "eq_bass=" << dkr::runtime::platform::bass_gain() << '\n';
        output << "eq_mid=" << dkr::runtime::platform::mid_gain() << '\n';
        output << "eq_treble=" << dkr::runtime::platform::treble_gain() << '\n';
        output << "maximum_detail="
               << (dkr::runtime::enhancements::maximum_detail_requested() ? 1 : 0) << '\n';
        output << "modern_fov_offset="
               << dkr::runtime::enhancements::fov_offset() << '\n';
        output << "magic_codes_persistent="
               << dkr::runtime::magic_codes::persistent_mask() << '\n';
        output << "modern_view_distance_multiplier="
               << dkr::runtime::enhancements::view_distance_multiplier() << '\n';
        output << "modern_keep_hub_scenery="
               << (dkr::runtime::enhancements::keep_hub_scenery_requested() ? 1 : 0)
               << '\n';
        output << "modern_extended_culling="
               << (dkr::runtime::enhancements::extended_culling_requested() ? 1 : 0)
               << '\n';
        output << "modern_frustum_guard_percent="
               << dkr::runtime::enhancements::frustum_guard_percent() << '\n';
        output << "memory_pak=" << (dkr::runtime::pak::enabled() ? 1 : 0) << '\n';
        output << "rumble=" << (dkr::runtime::platform::rumble_enabled() ? 1 : 0) << '\n';
        output << "rumble_strength=" << dkr::runtime::platform::rumble_strength() << '\n';
        output << "stick_deadzone=" << dkr::runtime::input::stick_deadzone() << '\n';
        output << "stick_anti_deadzone=" << dkr::runtime::input::stick_anti_deadzone() << '\n';
        output << "stick_sensitivity=" << dkr::runtime::input::stick_sensitivity() << '\n';
        output << "stick_curve=" << dkr::runtime::input::stick_curve() << '\n';
        output << "stick_x_inverted=" << (dkr::runtime::input::stick_x_inverted() ? 1 : 0) << '\n';
        output << "stick_y_inverted=" << (dkr::runtime::input::stick_y_inverted() ? 1 : 0) << '\n';
        output << "trigger_threshold=" << dkr::runtime::input::trigger_threshold() << '\n';
        const auto quick_keyboard =
            dkr::runtime::input::quick_restart_keyboard_binding();
        const auto quick_controller =
            dkr::runtime::input::quick_restart_controller_binding();
        output << "quick_restart_enabled="
               << (dkr::runtime::input::quick_restart_enabled() ? 1 : 0) << '\n';
        output << "quick_restart_keyboard_primary=" << quick_keyboard.primary << '\n';
        output << "quick_restart_keyboard_secondary=" << quick_keyboard.secondary << '\n';
        output << "quick_restart_controller_primary=" << quick_controller.primary << '\n';
        output << "quick_restart_controller_secondary=" << quick_controller.secondary << '\n';
        output << "gyro_enabled=" << (dkr::runtime::input::gyro_enabled() ? 1 : 0) << '\n';
        output << "gyro_sensitivity=" << dkr::runtime::input::gyro_sensitivity() << '\n';
        output << "gyro_y_sensitivity=" << dkr::runtime::input::gyro_y_sensitivity() << '\n';
        output << "gyro_deadzone=" << dkr::runtime::input::gyro_deadzone() << '\n';
        output << "gyro_inverted=" << (dkr::runtime::input::gyro_inverted() ? 1 : 0) << '\n';
        output << "gyro_y_inverted=" << (dkr::runtime::input::gyro_y_inverted() ? 1 : 0) << '\n';
        output << "gyro_axis=" << static_cast<int>(dkr::runtime::input::gyro_axis()) << '\n';
        for (std::size_t index = 0; index < dkr::runtime::input::action_count(); ++index) {
            const auto action = static_cast<dkr::runtime::input::Action>(index);
            output << "keyboard_binding." << dkr::runtime::input::action_identifier(action)
                   << '=' << dkr::runtime::input::keyboard_binding(action) << '\n';
            output << "controller_binding." << dkr::runtime::input::action_identifier(action)
                   << '=' << dkr::runtime::input::controller_binding(action) << '\n';
        }
        // This must be the final record. A truncated settings file is never allowed
        // to reactivate experimental presentation features.
        output << "settings_complete=1\n";
        output.flush();
        if (!output) {
            std::fprintf(stderr, "[boot][settings] failed while writing settings\n");
            output.close();
            std::filesystem::remove(temporary_path, error);
            return;
        }
    }
    if (!ReplaceSettingsFile(temporary_path, settings_path)) {
        std::fprintf(stderr, "[boot][settings] failed to replace settings file\n");
        std::filesystem::remove(temporary_path, error);
    }
}

void LoadSettings() {
    GraphicsConfig config = ultramodern::renderer::get_graphics_config();
    int settings_version = 0;
    bool settings_complete = false;
    auto profile = dkr::runtime::enhancements::PresentationProfile::Accurate;
    bool profile_value_valid = true;
    bool migrated = false;
    auto quick_keyboard = dkr::runtime::input::quick_restart_keyboard_binding();
    auto quick_controller = dkr::runtime::input::quick_restart_controller_binding();
    std::ifstream input(SettingsPath());
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        try {
            const int number = std::stoi(value);
            if (key == "settings_version") {
                settings_version = number;
            } else if (key == "settings_complete") {
                settings_complete = number == 1;
            } else if (key == "presentation_profile") {
                profile = dkr::runtime::enhancements::normalise_presentation_profile(number);
                profile_value_valid =
                    number == static_cast<int>(
                        dkr::runtime::enhancements::PresentationProfile::Accurate) ||
                    number == static_cast<int>(
                        dkr::runtime::enhancements::PresentationProfile::Modern);
            } else if (key == "window_mode" && number >= 0 && number < 2) {
                config.wm_option = static_cast<WindowMode>(number);
            } else if (key == "resolution" && number >= 0 && number < 3) {
                config.res_option = static_cast<Resolution>(number);
            } else if (key == "aspect" && number >= 0 && number < 2) {
                config.ar_option = static_cast<AspectRatio>(number);
            } else if (key == "antialiasing" && number >= 0 && number < 4) {
                config.msaa_option = static_cast<Antialiasing>(number);
            } else if (key == "high_precision_fb" && number >= 0 && number < 3) {
                config.hpfb_option = static_cast<HighPrecisionFramebuffer>(number);
            } else if (key == "graphics_api" && number >= 0 && number < 4) {
                config.api_option = static_cast<GraphicsApi>(number);
            } else if (key == "refresh_rate" && number >= 0 && number < 3) {
                config.rr_option = static_cast<RefreshRate>(number);
            } else if (key == "refresh_rate_target") {
                config.rr_manual_value =
                    dkr::runtime::enhancements::clamp_presentation_rate(number);
            } else if (key == "modern_refresh_rate" &&
                       number >= static_cast<int>(RefreshRate::Display) &&
                       number <= static_cast<int>(RefreshRate::Manual)) {
                g_modern_refresh_mode = static_cast<RefreshRate>(number);
            } else if (key == "modern_refresh_target") {
                g_modern_refresh_target =
                    dkr::runtime::enhancements::clamp_presentation_rate(number);
            } else if (key == "modern_resolution" && number >= 0 && number < 3) {
                g_modern_resolution = static_cast<Resolution>(number);
            } else if (key == "modern_aspect" && number >= 0 && number < 2) {
                g_modern_aspect = static_cast<AspectRatio>(number);
            } else if (key == "modern_antialiasing" && number >= 0 && number < 4) {
                g_modern_antialiasing = static_cast<Antialiasing>(number);
            } else if (key == "modern_high_precision_fb" && number >= 0 && number < 3) {
                g_modern_high_precision_fb =
                    static_cast<HighPrecisionFramebuffer>(number);
            } else if (key == "modern_graphics_api" && number >= 0 && number < 4) {
                g_modern_graphics_api = static_cast<GraphicsApi>(number);
            } else if (key == "modern_downsample") {
                g_modern_downsample = std::clamp(number, 1, 8);
            } else if (key == "modern_anisotropy") {
                dkr::runtime::enhancements::set_anisotropy_level(number);
            } else if (key == "fps_overlay_enabled") {
                g_fps_overlay_enabled = number != 0;
            } else if (key == "fps_overlay_position") {
                g_fps_overlay_position = std::clamp(number, 0, 3);
            } else if (key == "fps_overlay_detail") {
                g_fps_overlay_detail = std::clamp(number, 0, 3);
            } else if (key == "fps_overlay_single_row") {
                g_fps_overlay_single_row = number != 0;
            } else if (key == "fps_custom_frame_time") {
                g_fps_custom_frame_time = number != 0;
            } else if (key == "fps_custom_simulation") {
                g_fps_custom_simulation = number != 0;
            } else if (key == "fps_custom_graphics") {
                g_fps_custom_graphics = number != 0;
            } else if (key == "fps_custom_vi") {
                g_fps_custom_vi = number != 0;
            } else if (key == "fps_custom_interpolation") {
                g_fps_custom_interpolation = number != 0;
            } else if (key == "fps_custom_audio") {
                g_fps_custom_audio = number != 0;
            } else if (key == "fps_custom_target") {
                g_fps_custom_target = number != 0;
            } else if (key == "fps_custom_resolution") {
                g_fps_custom_resolution = number != 0;
            } else if (key == "fps_font_size") {
                g_fps_font_size = std::clamp(number, 16, 64);
            } else if (key == "fps_fill_r") {
                g_fps_fill_colour.x = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_fill_g") {
                g_fps_fill_colour.y = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_fill_b") {
                g_fps_fill_colour.z = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_fill_a") {
                g_fps_fill_colour.w = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_outline_r") {
                g_fps_outline_colour.x = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_outline_g") {
                g_fps_outline_colour.y = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_outline_b") {
                g_fps_outline_colour.z = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "fps_outline_a") {
                g_fps_outline_colour.w = std::clamp(number, 0, 255) / 255.0F;
            } else if (key == "crt_enabled") {
                g_crt_enabled = number != 0;
            } else if (key == "crt_filter_index") {
                g_crt_filter_index = std::max(number, 0);
            } else if (key == "crt_scale_mode") {
                g_crt_scale_mode = std::clamp(number, 0, 1);
            } else if (key == "crt_strength") {
                g_crt_strength = std::clamp(number, 0, 1000) / 1000.0F;
            } else if (key == "master_volume") {
                dkr::runtime::platform::set_master_volume(std::stof(value));
            } else if (key == "music_volume") {
                dkr::runtime::audio::set_music_volume(std::stof(value));
            } else if (key == "sound_effects_volume") {
                dkr::runtime::audio::set_sound_effects_volume(std::stof(value));
            } else if (key == "vehicle_volume") {
                dkr::runtime::audio::set_vehicle_volume(std::stof(value));
            } else if (key == "nature_volume") {
                dkr::runtime::audio::set_nature_volume(std::stof(value));
            } else if (key == "eq_bass") {
                dkr::runtime::platform::set_bass_gain(std::stof(value));
            } else if (key == "eq_mid") {
                dkr::runtime::platform::set_mid_gain(std::stof(value));
            } else if (key == "eq_treble") {
                dkr::runtime::platform::set_treble_gain(std::stof(value));
            } else if (key == "maximum_detail") {
                dkr::runtime::enhancements::set_maximum_detail_enabled(number != 0);
            } else if (key == "modern_fov_offset") {
                dkr::runtime::enhancements::set_fov_offset(number);
            } else if (key == "magic_codes_persistent") {
                dkr::runtime::magic_codes::set_persistent_mask(
                    static_cast<std::uint32_t>(std::max(number, 0)));
            } else if (key == "modern_view_distance_multiplier") {
                dkr::runtime::enhancements::set_view_distance_multiplier(number);
            } else if (key == "modern_keep_hub_scenery") {
                dkr::runtime::enhancements::set_keep_hub_scenery_enabled(number != 0);
            } else if (key == "modern_extended_culling") {
                dkr::runtime::enhancements::set_extended_culling_enabled(number != 0);
            } else if (key == "modern_frustum_guard_percent") {
                dkr::runtime::enhancements::set_frustum_guard_percent(number);
            } else if (key == "memory_pak") {
                dkr::runtime::pak::set_enabled(number != 0);
            } else if (key == "rumble") {
                dkr::runtime::platform::set_rumble_enabled(number != 0);
            } else if (key == "rumble_strength") {
                dkr::runtime::platform::set_rumble_strength(std::stof(value));
            } else if (key == "stick_deadzone") {
                dkr::runtime::input::set_stick_deadzone(std::stof(value));
            } else if (key == "stick_anti_deadzone") {
                dkr::runtime::input::set_stick_anti_deadzone(std::stof(value));
            } else if (key == "stick_sensitivity") {
                dkr::runtime::input::set_stick_sensitivity(std::stof(value));
            } else if (key == "stick_curve") {
                dkr::runtime::input::set_stick_curve(std::stof(value));
            } else if (key == "stick_x_inverted") {
                dkr::runtime::input::set_stick_x_inverted(number != 0);
            } else if (key == "stick_y_inverted") {
                dkr::runtime::input::set_stick_y_inverted(number != 0);
            } else if (key == "trigger_threshold") {
                dkr::runtime::input::set_trigger_threshold(std::stof(value));
            } else if (key == "quick_restart_enabled") {
                dkr::runtime::input::set_quick_restart_enabled(number != 0);
            } else if (key == "quick_restart_keyboard_primary") {
                quick_keyboard.primary = number;
            } else if (key == "quick_restart_keyboard_secondary") {
                quick_keyboard.secondary = number;
            } else if (key == "quick_restart_controller_primary") {
                quick_controller.primary = number;
            } else if (key == "quick_restart_controller_secondary") {
                quick_controller.secondary = number;
            } else if (key == "gyro_enabled") {
                dkr::runtime::input::set_gyro_enabled(number != 0);
            } else if (key == "gyro_sensitivity") {
                dkr::runtime::input::set_gyro_sensitivity(std::stof(value));
            } else if (key == "gyro_y_sensitivity") {
                dkr::runtime::input::set_gyro_y_sensitivity(std::stof(value));
            } else if (key == "gyro_deadzone") {
                dkr::runtime::input::set_gyro_deadzone(std::stof(value));
            } else if (key == "gyro_inverted") {
                dkr::runtime::input::set_gyro_inverted(number != 0);
            } else if (key == "gyro_y_inverted") {
                dkr::runtime::input::set_gyro_y_inverted(number != 0);
            } else if (key == "gyro_axis") {
                dkr::runtime::input::set_gyro_axis(
                    number == static_cast<int>(dkr::runtime::input::GyroAxis::Yaw)
                        ? dkr::runtime::input::GyroAxis::Yaw
                        : dkr::runtime::input::GyroAxis::Roll);
            } else if (key.rfind("keyboard_binding.", 0) == 0 ||
                       key.rfind("controller_binding.", 0) == 0) {
                const bool keyboard = key.rfind("keyboard_binding.", 0) == 0;
                const std::size_t prefix = keyboard ? 17U : 19U;
                const std::string identifier = key.substr(prefix);
                for (std::size_t index = 0;
                     index < dkr::runtime::input::action_count(); ++index) {
                    const auto action = static_cast<dkr::runtime::input::Action>(index);
                    if (identifier == dkr::runtime::input::action_identifier(action)) {
                        if (keyboard) {
                            dkr::runtime::input::set_keyboard_binding(action, number);
                        } else {
                            dkr::runtime::input::set_controller_binding(action, number);
                        }
                        break;
                    }
                }
            }
        } catch (...) {
            std::fprintf(stderr, "[boot][settings] ignored malformed setting %s\n", key.c_str());
        }
    }
    // Windows cannot atomically replace the settings file while this reader
    // still owns an open handle. Migration may call SaveSettings below, so
    // release the read handle before attempting that replacement.
    input.close();
    dkr::runtime::input::set_quick_restart_keyboard_binding(quick_keyboard);
    dkr::runtime::input::set_quick_restart_controller_binding(quick_controller);
    // Early launcher builds defaulted to 8x MSAA, which can turn busy races
    // GPU-bound at high desktop resolutions. Migrate that one legacy default
    // to 2x; users can still explicitly choose 4x or 8x afterwards.
    if (settings_version < 2 &&
        config.msaa_option == Antialiasing::MSAA8X) {
        config.msaa_option = Antialiasing::MSAA2X;
        migrated = true;
        std::fprintf(stderr, "[boot][settings] migrated legacy MSAA 8x default to 2x\n");
    }
    const auto resolved_profile =
        dkr::runtime::enhancements::resolve_settings_profile(
            settings_version, settings_complete, profile);
    if (settings_version <
            dkr::runtime::enhancements::kOldestCompatibleSettingsVersion ||
        settings_version > dkr::runtime::enhancements::kCurrentSettingsVersion ||
        !settings_complete || !profile_value_valid) {
        // Every settings file from before the hardened profile boundary, and
        // every truncated v4 write, becomes Accurate. Preserve the old refresh
        // preference as Modern's remembered value without activating it.
        if (config.rr_option == RefreshRate::Display ||
            config.rr_option == RefreshRate::Manual) {
            g_modern_refresh_mode = config.rr_option;
            g_modern_refresh_target =
                dkr::runtime::enhancements::clamp_presentation_rate(
                    config.rr_manual_value);
        }
        RememberModernGraphics(config);
        profile = resolved_profile;
        migrated = true;
        if (settings_version >= 4 && !settings_complete) {
            std::fprintf(stderr,
                         "[boot][settings] incomplete settings file; "
                         "falling back to Accurate\n");
        }
    }
    if (settings_version == 6 && settings_complete && profile_value_valid) {
        // Version 7 adds only opt-in controls. Preserve the proven v6 profile
        // and initialise the new shortcut as disabled.
        dkr::runtime::input::set_quick_restart_enabled(false);
        migrated = true;
    }
    profile = resolved_profile;
    dkr::runtime::enhancements::set_presentation_profile(profile);
    ApplyProfileGraphics(config, profile);
    config.developer_mode = false;
    ultramodern::renderer::set_graphics_config(config);
    if (migrated) {
        SaveSettings();
    }
}

void SaveLastRom(const std::filesystem::path& path) {
    std::ofstream output(LastRomPath(), std::ios::trunc);
    output << PathUtf8(path);
}

std::optional<std::filesystem::path> LoadLastRom() {
    std::ifstream input(LastRomPath());
    std::string path;
    std::getline(input, path);
    if (path.empty()) {
        return std::nullopt;
    }
    return std::filesystem::u8path(path);
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsRomFile(const std::filesystem::path& path) {
    const std::string extension = Lowercase(PathUtf8(path.extension()));
    return extension == ".z64" || extension == ".v64" || extension == ".n64";
}

std::filesystem::path DefaultBrowserDirectory(const std::filesystem::path& selected) {
    std::error_code error;
    if (!selected.empty()) {
        const auto parent = selected.parent_path();
        if (std::filesystem::is_directory(parent, error)) {
            return parent;
        }
    }
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr) {
        const auto downloads = std::filesystem::path(profile) / "Downloads";
        if (std::filesystem::is_directory(downloads, error)) {
            return downloads;
        }
        return std::filesystem::path(profile);
    }
#else
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        const auto downloads = std::filesystem::path(home) / "Downloads";
        if (std::filesystem::is_directory(downloads, error)) {
            return downloads;
        }
        return std::filesystem::path(home);
    }
#endif
    return std::filesystem::current_path(error);
}

void RefreshRomBrowser() {
    g_rom_browser.entries.clear();
    g_rom_browser.message.clear();
    std::error_code error;
    if (g_rom_browser.directory.empty()) {
#if defined(_WIN32)
        const DWORD drive_mask = GetLogicalDrives();
        for (int drive = 0; drive < 26; ++drive) {
            if ((drive_mask & (1UL << drive)) != 0) {
                std::string root = "A:\\";
                root[0] = static_cast<char>('A' + drive);
                g_rom_browser.entries.push_back({std::filesystem::path(root), true});
            }
        }
#else
        g_rom_browser.entries.push_back({std::filesystem::path("/"), true});
        if (const char* home = std::getenv("HOME"); home != nullptr) {
            g_rom_browser.entries.push_back({std::filesystem::path(home), true});
        }
#endif
    } else {
        std::filesystem::directory_iterator iterator(
            g_rom_browser.directory,
            std::filesystem::directory_options::skip_permission_denied, error);
        if (error) {
            g_rom_browser.message = "T.T. could not read this location: " + error.message();
        } else {
            for (const auto& item : iterator) {
                const bool directory = item.is_directory(error);
                error.clear();
                if (directory || (item.is_regular_file(error) && IsRomFile(item.path()))) {
                    g_rom_browser.entries.push_back({item.path(), directory});
                }
                error.clear();
            }
        }
    }
    std::sort(g_rom_browser.entries.begin(), g_rom_browser.entries.end(),
              [](const BrowserEntry& left, const BrowserEntry& right) {
        if (left.directory != right.directory) {
            return left.directory > right.directory;
        }
        return Lowercase(PathUtf8(left.path.filename())) <
               Lowercase(PathUtf8(right.path.filename()));
    });
    g_rom_browser.focus_first_entry = true;
}

void OpenRomBrowser(const std::filesystem::path& selected) {
    g_rom_browser.open = true;
    g_rom_browser.close_requested = false;
    g_rom_browser.directory = DefaultBrowserDirectory(selected);
    RefreshRomBrowser();
}

void RomBrowserBack() {
    if (!g_rom_browser.open) {
        return;
    }
    if (g_rom_browser.directory.empty()) {
        g_rom_browser.close_requested = true;
        return;
    }
    const auto parent = g_rom_browser.directory.parent_path();
    if (parent.empty() || parent == g_rom_browser.directory) {
        g_rom_browser.directory.clear();
    } else {
        g_rom_browser.directory = parent;
    }
    RefreshRomBrowser();
}

bool AcceptRom(const std::filesystem::path& path, std::filesystem::path& selected,
               std::string& status) {
    std::string error;
    if (!dkr::runtime::SelectRom(path, error)) {
        status = error;
        g_rom_browser.message = error;
        return false;
    }
    selected = path;
    SaveLastRom(selected);
    status = "Game Pak ready. The adventure can begin!";
    g_rom_browser.open = false;
    g_rom_browser.close_requested = false;
    return true;
}

bool SelectRomWithDialog(std::filesystem::path& selected, std::string& status) {
    if (NFD_Init() != NFD_OKAY) {
        status = "The system file picker could not be initialized.";
        return false;
    }
    nfdu8char_t* result = nullptr;
    const nfdfilteritem_t filters[] = {{"Nintendo 64 ROM", "z64,v64,n64"}};
    const nfdresult_t dialog = NFD_OpenDialogU8(&result, filters, 1, nullptr);
    if (dialog == NFD_OKAY) {
        selected = std::filesystem::u8path(result);
        NFD_FreePathU8(result);
        std::string error;
        if (!dkr::runtime::SelectRom(selected, error)) {
            status = error;
            NFD_Quit();
            return false;
        }
        SaveLastRom(selected);
        status = "Game Pak ready. The adventure can begin!";
        NFD_Quit();
        return true;
    }
    if (dialog == NFD_ERROR) {
        status = NFD_GetError();
    }
    NFD_Quit();
    return false;
}

bool ImportCrtFilterWithDialog() {
    // The Vulkan Inspector pool reserves one descriptor for the font atlas and
    // provides 63 image descriptors. Six are used by the built-in masks, so
    // cap the imported catalogue before a live session can exhaust the pool.
    // Uploaded masks are deliberately retained until renderer shutdown to
    // avoid freeing an image that an in-flight presentation still references.
    constexpr std::size_t maximum_crt_filters = 63;
    if (g_crt_filters.size() >= maximum_crt_filters) {
        g_crt_status =
            "The CRT filter library is full. Remove a custom filter from the "
            "local filters folder before importing another.";
        return false;
    }
    if (NFD_Init() != NFD_OKAY) {
        g_crt_status = "The system file picker could not be initialized.";
        return false;
    }
    nfdu8char_t* result = nullptr;
    const nfdfilteritem_t filters[] = {{"PNG image", "png"}};
    const nfdresult_t dialog = NFD_OpenDialogU8(&result, filters, 1, nullptr);
    if (dialog != NFD_OKAY) {
        if (dialog == NFD_ERROR) g_crt_status = NFD_GetError();
        NFD_Quit();
        return false;
    }
    const std::filesystem::path source = std::filesystem::u8path(result);
    NFD_FreePathU8(result);
    NFD_Quit();

    std::error_code error;
    constexpr std::uintmax_t maximum_filter_bytes = 64U * 1024U * 1024U;
    const std::uintmax_t size = std::filesystem::file_size(source, error);
    if (error || size == 0 || size > maximum_filter_bytes) {
        g_crt_status = "Choose a valid PNG filter no larger than 64 MB.";
        return false;
    }
    const std::filesystem::path custom_directory = g_config_directory / "filters";
    std::filesystem::create_directories(custom_directory, error);
    if (error) {
        g_crt_status = "The custom filter folder could not be created.";
        return false;
    }
    std::string stem = source.stem().string();
    if (stem.empty()) stem = "custom-filter";
    std::filesystem::path destination = custom_directory / (stem + ".png");
    for (int suffix = 2; std::filesystem::exists(destination, error) && suffix < 1000;
         ++suffix) {
        destination = custom_directory /
            (stem + "-" + std::to_string(suffix) + ".png");
    }
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::none, error);
    if (error) {
        g_crt_status = "The custom filter could not be imported: " + error.message();
        return false;
    }
    RefreshCrtFilters();
    for (std::size_t index = 0; index < g_crt_filters.size(); ++index) {
        if (g_crt_filters[index].path == destination) {
            g_crt_filter_index = static_cast<int>(index);
            break;
        }
    }
    g_crt_enabled = true;
    g_crt_status = "Imported " + destination.filename().string() + ".";
    SaveSettings();
    return true;
}

bool ImportTexturePackWithDialog() {
    if (NFD_Init() != NFD_OKAY) {
        g_texture_pack_status = "The system file picker could not be initialized.";
        return false;
    }
    nfdu8char_t* result = nullptr;
    const nfdfilteritem_t filters[] = {
        {"Texture-pack archive", "zip,rtz"},
    };
    const nfdresult_t dialog = NFD_OpenDialogU8(&result, filters, 1, nullptr);
    if (dialog != NFD_OKAY) {
        if (dialog == NFD_ERROR) g_texture_pack_status = NFD_GetError();
        NFD_Quit();
        return false;
    }
    const std::filesystem::path source = std::filesystem::u8path(result);
    NFD_FreePathU8(result);
    NFD_Quit();
    std::string status;
    const bool imported =
        dkr::runtime::texture_packs::import_archive(source, status);
    g_texture_pack_status = status;
    return imported;
}

bool ImportAdventureWithDialog() {
    if (NFD_Init() != NFD_OKAY) {
        g_save_manager_status = "The system file picker could not be initialized.";
        return false;
    }
    nfdu8char_t* result = nullptr;
    const nfdfilteritem_t filters[] = {{"DKR Adventure save", "bin"}};
    const nfdresult_t dialog = NFD_OpenDialogU8(&result, filters, 1, nullptr);
    bool imported = false;
    if (dialog == NFD_OKAY) {
        const auto source = std::filesystem::u8path(result);
        NFD_FreePathU8(result);
        std::string error;
        imported = dkr::runtime::saves::import_adventure(source, error);
        if (imported) {
            g_save_builder_image.reset();
        }
        g_save_manager_status = imported
            ? "Adventure save imported. The previous save was backed up first."
            : error;
    } else if (dialog == NFD_ERROR) {
        g_save_manager_status = NFD_GetError();
    }
    NFD_Quit();
    return imported;
}

bool ExportAdventureWithDialog() {
    if (NFD_Init() != NFD_OKAY) {
        g_save_manager_status = "The system file picker could not be initialized.";
        return false;
    }
    nfdu8char_t* result = nullptr;
    const nfdfilteritem_t filters[] = {{"DKR Adventure save", "bin"}};
    const nfdresult_t dialog = NFD_SaveDialogU8(
        &result, filters, 1, nullptr, "dkr-adventure-save.bin");
    bool exported = false;
    if (dialog == NFD_OKAY) {
        auto destination = std::filesystem::u8path(result);
        NFD_FreePathU8(result);
        if (destination.extension().empty()) {
            destination += ".bin";
        }
        std::string error;
        exported = dkr::runtime::saves::export_adventure(destination, error);
        g_save_manager_status = exported
            ? "Adventure save exported successfully."
            : error;
    } else if (dialog == NFD_ERROR) {
        g_save_manager_status = NFD_GetError();
    }
    NFD_Quit();
    return exported;
}

bool ImportSaveBundleWithDialog() {
    if (NFD_Init() != NFD_OKAY) {
        g_save_manager_status = "The system file picker could not be initialized.";
        return false;
    }
    nfdchar_t* result = nullptr;
    const nfdfilteritem_t filters[] = {{"DKR-R save bundle", "dkrsave"}};
    const nfdresult_t dialog = NFD_OpenDialogU8(&result, filters, 1, nullptr);
    bool imported = false;
    if (dialog == NFD_OKAY) {
        const auto source = std::filesystem::u8path(result);
        NFD_FreePathU8(result);
        std::string error;
        imported = dkr::runtime::saves::import_bundle(source, error);
        if (imported) {
            g_save_builder_image.reset();
        }
        g_save_manager_status = imported
            ? "Adventure and Controller Pak saves returned safely to T.T.'s garage."
            : error;
    } else if (dialog == NFD_ERROR) {
        g_save_manager_status = NFD_GetError();
    }
    NFD_Quit();
    return imported;
}

bool ExportSaveBundleWithDialog() {
    if (NFD_Init() != NFD_OKAY) {
        g_save_manager_status = "The system file picker could not be initialized.";
        return false;
    }
    nfdchar_t* result = nullptr;
    const nfdfilteritem_t filters[] = {{"DKR-R save bundle", "dkrsave"}};
    const nfdresult_t dialog = NFD_SaveDialogU8(
        &result, filters, 1, nullptr, "dkr-port-save-garage.dkrsave");
    bool exported = false;
    if (dialog == NFD_OKAY) {
        auto destination = std::filesystem::u8path(result);
        NFD_FreePathU8(result);
        if (destination.extension().empty()) {
            destination += ".dkrsave";
        }
        std::string error;
        exported = dkr::runtime::saves::export_bundle(destination, error);
        g_save_manager_status = exported
            ? "Complete save garage exported successfully."
            : error;
    } else if (dialog == NFD_ERROR) {
        g_save_manager_status = NFD_GetError();
    }
    NFD_Quit();
    return exported;
}

void DrawRaceBadge(const char* label, const ImVec4& color, float width = 0.0F);

bool BeginPaddedChild(const char* id, const ImVec2& size, bool border = true,
                      ImGuiWindowFlags flags = 0,
                      const ImVec2& padding = {18.0F, 16.0F}) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    const bool visible = ImGui::BeginChild(id, size, border, flags);
    ImGui::PopStyleVar();
    return visible;
}

bool BeginPaddedModal(const char* name, ImGuiWindowFlags flags = 0,
                      const ImVec2& padding = {26.0F, 24.0F}) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0F);
    const bool visible = ImGui::BeginPopupModal(name, nullptr, flags);
    ImGui::PopStyleVar(2);
    return visible;
}

void DrawRomBrowser(std::filesystem::path& selected, std::string& status,
                    bool& rom_ready) {
    constexpr const char* kPopupName = "Select Diddy Kong Racing ROM";
    if (g_rom_browser.open && !ImGui::IsPopupOpen(kPopupName)) {
        ImGui::OpenPopup(kPopupName);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        {std::clamp(viewport->Size.x * 0.72F, 680.0F, 1040.0F),
         std::clamp(viewport->Size.y * 0.78F, 560.0F, 760.0F)},
        ImGuiCond_Appearing);
    const bool popup_visible = BeginPaddedModal(
        kPopupName, ImGuiWindowFlags_NoSavedSettings);
    if (!popup_visible) {
        return;
    }
    if (g_rom_browser.close_requested) {
        g_rom_browser.open = false;
        g_rom_browser.close_requested = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    PushHeadingFont();
    ImGui::TextUnformatted("SELECT DIDDY KONG RACING ROM");
    PopHeadingFont();
    ImGui::TextDisabled("Folders first, then .z64, .v64 and .n64 files. Nothing leaves this PC.");
    ImGui::Dummy({0.0F, 8.0F});

    const std::string location = g_rom_browser.directory.empty()
        ? "This PC"
        : PathUtf8(g_rom_browser.directory);
    ImGui::TextWrapped("Current folder: %s", location.c_str());
    if (ImGui::Button("UP ONE LEVEL", {170.0F, 42.0F})) {
        RomBrowserBack();
    }
    ImGui::SameLine();
    if (ImGui::Button("THIS PC", {130.0F, 42.0F})) {
        g_rom_browser.directory.clear();
        RefreshRomBrowser();
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.02F, 0.12F, 0.23F, 0.98F});
    BeginPaddedChild("game-pak-files", {0.0F, -118.0F}, true,
                     ImGuiWindowFlags_NavFlattened, {14.0F, 12.0F});
    if (!g_rom_browser.message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
        ImGui::TextWrapped("%s", g_rom_browser.message.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    if (g_rom_browser.entries.empty() && g_rom_browser.message.empty()) {
        ImGui::TextDisabled("No supported ROM files or folders were found here.");
    }
    for (std::size_t index = 0; index < g_rom_browser.entries.size(); ++index) {
        const BrowserEntry& entry = g_rom_browser.entries[index];
        std::string name = PathUtf8(entry.path.filename());
        if (name.empty()) {
            name = PathUtf8(entry.path);
        }
        const std::string label = entry.directory
            ? "[FOLDER]  " + name
            : "[ROM]     " + name;
        if (ImGui::Selectable(label.c_str(), false,
                              ImGuiSelectableFlags_SpanAllColumns, {0.0F, 38.0F})) {
            if (entry.directory) {
                g_rom_browser.directory = entry.path;
                RefreshRomBrowser();
                break;
            }
            rom_ready = AcceptRom(entry.path, selected, status);
            if (rom_ready) {
                ImGui::CloseCurrentPopup();
                break;
            }
        }
        if (g_rom_browser.focus_first_entry && index == 0) {
            ImGui::SetItemDefaultFocus();
            g_rom_browser.focus_first_entry = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::TextDisabled("A / Cross  SELECT     B / Circle  BACK     D-pad / Stick  MOVE");
    if (ImGui::Button("CANCEL", {130.0F, 42.0F})) {
        g_rom_browser.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("SYSTEM FILE PICKER", {220.0F, 42.0F})) {
        rom_ready = SelectRomWithDialog(selected, status);
        if (rom_ready) {
            g_rom_browser.open = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

bool BeginMainWindow(const char* name, ImGuiWindowFlags extra = 0) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    return ImGui::Begin(name, nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | extra);
}

ImU32 MixCheckerColor(const ImVec4& left, const ImVec4& right, float amount,
                      float brightness, float alpha) {
    const auto mix = [&](float a, float b) {
        return std::clamp((a + (b - a) * amount) * brightness, 0.0F, 1.0F);
    };
    return ImGui::ColorConvertFloat4ToU32(
        {mix(left.x, right.x), mix(left.y, right.y), mix(left.z, right.z), alpha});
}

void DrawRaceBackdrop(bool overlay) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float cell = std::clamp(std::min(size.x, size.y) * 0.055F, 34.0F, 68.0F);
    const int columns = static_cast<int>(size.x / cell) + 1;
    const int rows = static_cast<int>(size.y / cell) + 1;
    const ImVec4 blue{0.025F, 0.25F, 0.66F, 1.0F};
    const ImVec4 orange{0.98F, 0.34F, 0.055F, 1.0F};
    const float alpha = overlay ? 0.92F : 1.0F;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const ImVec2 cell_min{origin.x + column * cell,
                                  origin.y + row * cell};
            const ImVec2 cell_max{
                std::min(cell_min.x + cell, origin.x + size.x),
                std::min(cell_min.y + cell, origin.y + size.y)};
            const float amount = std::clamp(
                ((cell_min.x + cell_max.x) * 0.5F - origin.x) /
                    std::max(size.x, 1.0F),
                0.0F, 1.0F);
            const float brightness = ((row + column) & 1) ? 0.72F : 1.0F;
            draw->AddRectFilled(cell_min, cell_max,
                MixCheckerColor(blue, orange, amount, brightness, alpha));
        }
    }
}

void DrawRaceBadge(const char* label, const ImVec4& color, float width) {
    // Badges communicate status; they are deliberately not ImGui buttons so
    // keyboard/gamepad navigation never wastes a stop on non-actions.
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 badge_size{
        width > 0.0F ? width : text_size.x + 20.0F,
        30.0F};
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position,
        {position.x + badge_size.x, position.y + badge_size.y},
        ImGui::ColorConvertFloat4ToU32(color), 15.0F);
    draw->AddText({position.x + std::max((badge_size.x - text_size.x) * 0.5F, 0.0F),
                   position.y + std::max((badge_size.y - text_size.y) * 0.5F, 0.0F)},
                  ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::Dummy(badge_size);
}

void DrawStartingLights(bool ready) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(cursor, {cursor.x + 42.0F, cursor.y + 112.0F},
                        IM_COL32(12, 25, 34, 255), 21.0F);
    const ImU32 off = IM_COL32(78, 91, 91, 255);
    draw->AddCircleFilled({cursor.x + 21.0F, cursor.y + 24.0F}, 9.0F,
                          ready ? off : IM_COL32(236, 54, 39, 255));
    draw->AddCircleFilled({cursor.x + 21.0F, cursor.y + 56.0F}, 9.0F,
                          ready ? off : IM_COL32(255, 174, 24, 255));
    draw->AddCircleFilled({cursor.x + 21.0F, cursor.y + 88.0F}, 9.0F,
                          ready ? IM_COL32(32, 218, 129, 255) : off);
    ImGui::Dummy({42.0F, 112.0F});
}

void BrandBlock(float available_width, float maximum_size = 230.0F) {
    if (g_brand_logo_rect >= 0) {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        const ImFontAtlasCustomRect* rect =
            atlas->GetCustomRectByIndex(g_brand_logo_rect);
        if (rect != nullptr && rect->IsPacked() && atlas->TexID != nullptr &&
            atlas->TexWidth > 0 && atlas->TexHeight > 0) {
            constexpr float kContainerPadding = 8.0F;
            const float container_width = std::min(available_width, maximum_size);
            const float width = std::max(container_width - kContainerPadding * 2.0F, 1.0F);
            const float height = width * static_cast<float>(rect->Height) /
                static_cast<float>(rect->Width);
            const float container_height = height + kContainerPadding * 2.0F;
            const float indent = std::max((available_width - container_width) * 0.5F, 0.0F);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            const ImVec2 container_min = ImGui::GetCursorScreenPos();
            const ImVec2 container_max{container_min.x + container_width,
                                       container_min.y + container_height};
            ImVec2 uv_min{};
            ImVec2 uv_max{};
            atlas->CalcCustomRectUV(rect, &uv_min, &uv_max);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(container_min, container_max,
                                IM_COL32(3, 29, 44, 238), 20.0F);
            draw->AddRect(container_min, container_max,
                          ImGui::ColorConvertFloat4ToU32(kWarm), 20.0F, 0, 2.0F);
            const ImVec2 image_min{container_min.x + kContainerPadding,
                                   container_min.y + kContainerPadding};
            const ImVec2 image_max{image_min.x + width, image_min.y + height};
            draw->AddImageRounded(atlas->TexID, image_min, image_max, uv_min, uv_max,
                                  IM_COL32_WHITE, 13.0F);
            ImGui::Dummy({container_width, container_height});
            return;
        }
    }

    // Keep startup usable when a development tree has not staged its visual
    // assets yet; packaged builds always carry DKR-R8.bmp beside the runtime.
    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
    PushHeadingFont();
    ImGui::TextUnformatted("DKR-R");
    PopHeadingFont();
    ImGui::PopStyleColor();
    PushHeadingFont(true);
    ImGui::TextWrapped("DIDDY KONG RACING RECOMPILED");
    PopHeadingFont(true);
}

bool SidebarButton(const char* label, int page, float width = -1.0F) {
    if (g_overlay_page == page) {
        ImGui::PushStyleColor(ImGuiCol_Button, page == 0 ? kAccent : kRaceRed);
    }
    const bool pressed = ImGui::Button(label, {width, 46.0F});
    if (g_overlay_page == page) {
        ImGui::PopStyleColor();
    }
    if (pressed) {
        g_overlay_page = page;
    }
    return pressed;
}

bool LauncherSidebarButton(const char* label, int target_page, int& page,
                           bool focus_selected, float width) {
    if (page == target_page) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              target_page == 0 ? kAccent : kRaceRed);
    }
    if (focus_selected && page == target_page) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool pressed = ImGui::Button(label, {width, 46.0F});
    if (page == target_page) {
        ImGui::PopStyleColor();
    }
    if (pressed) {
        page = target_page;
    }
    return pressed;
}

void DrawSidebarNote(float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.075F, 0.165F, 0.20F, 0.94F});
    BeginPaddedChild("sidebar-note", {width, 154.0F}, true,
                     ImGuiWindowFlags_NoScrollbar, {18.0F, 16.0F});
    ImGui::PushTextWrapPos(width - 18.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
    ImGui::TextUnformatted("NOTE");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Bring your own legally obtained Game Pak. It stays on this PC and is never uploaded.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawAboutDkrR(float width) {
    DrawPageHeading("ABOUT DKR-R");
    ImGui::TextDisabled("Diddy Kong Racing - Recompiled");
    ImGui::Dummy({0.0F, 14.0F});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.045F, 0.18F, 0.25F, 0.96F});
    BeginPaddedChild("about-dkr-r-card", {width, 360.0F}, true,
                     ImGuiWindowFlags_NoScrollbar, {22.0F, 20.0F});
    ImGui::PushTextWrapPos(width - 20.0F);
    ImGui::TextWrapped("Wizpig has invaded DKR-R. Race across land, water and sky, collect Golden Balloons and help Diddy and his friends send the intergalactic pig wizard packing.");
    ImGui::Dummy({0.0F, 10.0F});
    ImGui::TextWrapped("DKR-R runs the original game logic through a native PC runtime. Accurate preserves the original presentation; Modern adds carefully isolated PC quality-of-life options.");
    ImGui::Dummy({0.0F, 14.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
    ImGui::TextWrapped("CREATED BY THATGUYMCD");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("If you did not download DKR-R from ThatGuyMcd's GitHub repository, this build may have been modified or tampered with.");
    ImGui::TextDisabled("Official source: github.com/ThatGuyMcd/DKR-R");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 16.0F});
    ImGui::SeparatorText("Release information");
    ImGui::TextWrapped("DKR-R %s\nWindows, Linux and macOS builds",
                       DKR_RELEASE_VERSION);
    ImGui::Dummy({0.0F, 8.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("No copyrighted game data is distributed. A legally obtained supported Diddy Kong Racing Game Pak is required.");
    ImGui::PopStyleColor();
}

void DrawComingSoonPage(const char* heading, const char* card_id,
                        const char* description, const char* workshop_note,
                        float width) {
    DrawPageHeading(heading);
    ImGui::TextDisabled("A new route is being prepared for DKR-R.");
    ImGui::Dummy({0.0F, 16.0F});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.045F, 0.18F, 0.25F, 0.96F});
    BeginPaddedChild(card_id, {width, 286.0F}, true,
                     ImGuiWindowFlags_NoScrollbar, {24.0F, 22.0F});
    ImGui::PushTextWrapPos(std::max(width - 24.0F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
    PushHeadingFont(true);
    ImGui::TextUnformatted("COMING SOON");
    PopHeadingFont(true);
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 10.0F});
    ImGui::TextWrapped("%s", description);
    ImGui::Dummy({0.0F, 14.0F});
    ImGui::Separator();
    ImGui::Dummy({0.0F, 10.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", workshop_note);
    ImGui::TextWrapped("This feature is not finished cooking yet, so it stays safely parked for this release.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

bool DrawGraphicsSettings(bool live) {
    GraphicsConfig config = ultramodern::renderer::get_graphics_config();
    bool changed = false;
    bool profile_changed = false;
    int profile = static_cast<int>(
        dkr::runtime::enhancements::presentation_profile());
    int window = static_cast<int>(config.wm_option);
    int resolution = static_cast<int>(config.res_option);
    int aspect = static_cast<int>(config.ar_option);
    int aa = static_cast<int>(config.msaa_option);
    int hpfb = static_cast<int>(config.hpfb_option);
    int downsample = std::clamp(config.ds_option, 1, 4);
    const float available_width = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    const float setting_width = std::clamp(available_width * 0.92F,
                                           std::min(220.0F, available_width),
                                           std::min(920.0F, available_width));
    ImGui::TextUnformatted("PRESENTATION STYLE");
    ImGui::SetNextItemWidth(setting_width);
    profile_changed = ControlCombo("##presentation-profile", &profile,
                                   "Accurate\0Modern\0");
    if (profile_changed) {
        const auto old_profile = dkr::runtime::enhancements::presentation_profile();
        if (old_profile == dkr::runtime::enhancements::PresentationProfile::Modern) {
            RememberModernGraphics(config);
        }
        const auto next_profile =
            static_cast<dkr::runtime::enhancements::PresentationProfile>(profile);
        dkr::runtime::enhancements::set_presentation_profile(next_profile);
        ApplyProfileGraphics(config, next_profile);
        resolution = static_cast<int>(config.res_option);
        aspect = static_cast<int>(config.ar_option);
        aa = static_cast<int>(config.msaa_option);
        hpfb = static_cast<int>(config.hpfb_option);
        downsample = std::clamp(config.ds_option, 1, 4);
        changed = true;
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Window mode");
    ImGui::SetNextItemWidth(setting_width);
    changed |= ControlCombo("##window-mode", &window, "Windowed\0Fullscreen\0");
    config.wm_option = static_cast<WindowMode>(window);
    const bool modern_profile =
        dkr::runtime::enhancements::modern_options_visible(
            dkr::runtime::enhancements::presentation_profile());
    if (modern_profile) {
        ImGui::TextUnformatted("Internal resolution");
        ImGui::SetNextItemWidth(setting_width);
        changed |= ControlCombo("##internal-resolution", &resolution,
                                "Original (240p)\0Original 2x\0Automatic integer scale\0");
        ImGui::TextUnformatted("Aspect ratio");
        ImGui::SetNextItemWidth(setting_width);
        changed |= ControlCombo("##aspect-ratio", &aspect,
                                "Original 4:3\0Fit to window\0");
        ImGui::TextUnformatted("Anti-aliasing");
        ImGui::SetNextItemWidth(setting_width);
        changed |= ControlCombo("##anti-aliasing", &aa,
                                "None\0MSAA 2x\0MSAA 4x\0MSAA 8x\0");
        constexpr std::array<int, 5> kAnisotropyLevels{1, 2, 4, 8, 16};
        int anisotropy_index = 0;
        const int anisotropy =
            dkr::runtime::enhancements::anisotropy_level();
        for (std::size_t index = 0; index < kAnisotropyLevels.size(); ++index) {
            if (kAnisotropyLevels[index] == anisotropy) {
                anisotropy_index = static_cast<int>(index);
                break;
            }
        }
        ImGui::TextUnformatted("Anisotropic filtering");
        ImGui::SetNextItemWidth(setting_width);
        if (ControlCombo("##anisotropic-filtering", &anisotropy_index,
                         "1x (off)\0" "2x\0" "4x\0" "8x\0" "16x\0")) {
            dkr::runtime::enhancements::set_anisotropy_level(
                kAnisotropyLevels[static_cast<std::size_t>(anisotropy_index)]);
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("Improves angled track textures. Samplers are rebuilt on the next game launch.");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted("High precision framebuffer");
        ImGui::SetNextItemWidth(setting_width);
        changed |= ControlCombo("##high-precision-framebuffer", &hpfb,
                                "Automatic\0On\0Off\0");
        ImGui::TextUnformatted("Downsampling quality");
        ImGui::SetNextItemWidth(setting_width);
        if (ControlSliderInt("##downsample-quality", &downsample, 1, 4,
                             downsample == 1 ? "Off" : "%dx", ImGuiSliderFlags_AlwaysClamp)) {
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("Renders extra pixels before the final image is reduced. Higher values are expensive; 1x is recommended for high refresh rates.");
        ImGui::PopStyleColor();

        ImGui::TextUnformatted("Graphics API");
        ImGui::SetNextItemWidth(setting_width);
#if defined(_WIN32)
        int api_choice = config.api_option == GraphicsApi::D3D12
            ? 1
            : config.api_option == GraphicsApi::Vulkan ? 2 : 0;
        if (ControlCombo("##graphics-api", &api_choice,
                         "Automatic (recommended)\0Direct3D 12\0Vulkan\0")) {
            config.api_option = api_choice == 1
                ? GraphicsApi::D3D12
                : api_choice == 2 ? GraphicsApi::Vulkan : GraphicsApi::Auto;
            changed = true;
        }
#elif defined(__linux__)
        int api_choice = config.api_option == GraphicsApi::Vulkan ? 1 : 0;
        if (ControlCombo("##graphics-api", &api_choice,
                         "Automatic (recommended)\0Vulkan\0")) {
            config.api_option = api_choice == 1
                ? GraphicsApi::Vulkan
                : GraphicsApi::Auto;
            changed = true;
        }
#elif defined(__APPLE__)
        int api_choice = 0;
        ImGui::BeginDisabled();
        ControlCombo("##graphics-api", &api_choice,
                     "Metal (automatic)\0");
        ImGui::EndDisabled();
#else
        int api_choice = 0;
        ImGui::BeginDisabled();
        ControlCombo("##graphics-api", &api_choice, "Automatic\0");
        ImGui::EndDisabled();
#endif
        config.res_option = static_cast<Resolution>(resolution);
        config.ar_option = static_cast<AspectRatio>(aspect);
        config.msaa_option = static_cast<Antialiasing>(aa);
        config.hpfb_option = static_cast<HighPrecisionFramebuffer>(hpfb);
        config.hr_option = HUDRatioMode::Original;
        config.ds_option = downsample;
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.055F, 0.19F, 0.29F, 1.0F});
        BeginPaddedChild("accurate-aspect-lock", {setting_width, 96.0F}, true,
                         ImGuiWindowFlags_NoScrollbar, {18.0F, 15.0F});
        ImGui::PushTextWrapPos(std::max(setting_width - 18.0F, 1.0F));
        ImGui::TextUnformatted("Original 4:3 - Accurate");
        ImGui::TextWrapped("Fit to Window, graphics tuning and maximum vehicle detail appear only in Modern.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ApplyProfileGraphics(config,
                             dkr::runtime::enhancements::PresentationProfile::Accurate);
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Presentation rate");
    if (dkr::runtime::enhancements::modern_presentation_enabled()) {
        int refresh_mode = g_modern_refresh_mode == RefreshRate::Manual ? 1 : 0;
        ImGui::SetNextItemWidth(setting_width);
        if (ControlCombo("##modern-refresh-mode", &refresh_mode,
                         "Match display\0Manual target\0")) {
            g_modern_refresh_mode = refresh_mode == 0
                ? RefreshRate::Display
                : RefreshRate::Manual;
            config.rr_option = g_modern_refresh_mode;
            changed = true;
        }
        if (g_modern_refresh_mode == RefreshRate::Manual) {
            ImGui::TextUnformatted("Frame-rate target");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlSliderInt("##modern-refresh-target", &g_modern_refresh_target,
                                 30, 500, "%d FPS", ImGuiSliderFlags_AlwaysClamp)) {
                g_modern_refresh_target =
                    dkr::runtime::enhancements::clamp_presentation_rate(
                        g_modern_refresh_target);
                config.rr_manual_value = g_modern_refresh_target;
                changed = true;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("Targets above the monitor refresh can reduce input-to-present latency, but cannot add visible refreshes and use more CPU/GPU power.");
            ImGui::PopStyleColor();
        }
        config.rr_option = g_modern_refresh_mode;
        config.rr_manual_value = g_modern_refresh_target;
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.055F, 0.19F, 0.29F, 1.0F});
        BeginPaddedChild("accurate-presentation-rate", {setting_width, 96.0F}, true,
                         ImGuiWindowFlags_NoScrollbar, {18.0F, 15.0F});
        ImGui::PushTextWrapPos(std::max(setting_width - 18.0F, 1.0F));
        ImGui::TextUnformatted("Original 30 FPS - Accurate");
        ImGui::TextWrapped("Interpolation is locked off. Game, audio and presentation use the proven original cadence.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        config.rr_option = RefreshRate::Original;
        config.rr_manual_value = 30;
    }
    if (changed) {
        if (modern_profile) {
            RememberModernGraphics(config);
        }
        ultramodern::renderer::set_graphics_config(config);
        SaveSettings();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    if (!live) {
        ImGui::TextWrapped("Changes made before launch are applied when the adventure begins.");
    }
    ImGui::TextWrapped(modern_profile
        ? "Graphics API selection is applied at the next game launch. Automatic remains the recovery choice."
        : "Graphics API: Automatic. Accurate always uses the release-proven platform choice.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (modern_profile) {
        ImGui::SeparatorText("Race telemetry");
        bool fps_overlay = g_fps_overlay_enabled;
        if (ImGui::Checkbox("Show DKR-R performance overlay", &fps_overlay)) {
            g_fps_overlay_enabled = fps_overlay;
            SaveSettings();
            changed = true;
        }
        if (g_fps_overlay_enabled) {
            ImGui::TextUnformatted("Overlay position");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlCombo("##fps-overlay-position", &g_fps_overlay_position,
                             "Top left\0Top right\0Bottom left\0Bottom right\0")) {
                SaveSettings();
            }
            ImGui::TextUnformatted("Detail preset");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlCombo("##fps-overlay-detail", &g_fps_overlay_detail,
                             "FPS only\0Standard\0Detailed\0Custom\0")) {
                SaveSettings();
            }
            int fps_layout = g_fps_overlay_single_row ? 1 : 0;
            ImGui::TextUnformatted("Metric layout");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlCombo("##fps-overlay-layout", &fps_layout,
                             "Stacked\0Single row\0")) {
                g_fps_overlay_single_row = fps_layout == 1;
                SaveSettings();
            }
            if (g_fps_overlay_detail == 3) {
                bool custom_changed = false;
                custom_changed |= ImGui::Checkbox(
                    "Frame time", &g_fps_custom_frame_time);
                custom_changed |= ImGui::Checkbox(
                    "Simulation rate", &g_fps_custom_simulation);
                custom_changed |= ImGui::Checkbox(
                    "Graphics task rate", &g_fps_custom_graphics);
                custom_changed |= ImGui::Checkbox(
                    "VI rate", &g_fps_custom_vi);
                custom_changed |= ImGui::Checkbox(
                    "Interpolated frame rate", &g_fps_custom_interpolation);
                custom_changed |= ImGui::Checkbox(
                    "Audio sample rate", &g_fps_custom_audio);
                custom_changed |= ImGui::Checkbox(
                    "Presentation target", &g_fps_custom_target);
                custom_changed |= ImGui::Checkbox(
                    "Viewport resolution", &g_fps_custom_resolution);
                if (custom_changed) SaveSettings();
            }
            ImGui::TextUnformatted("Font size");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlSliderInt("##fps-font-size", &g_fps_font_size,
                                 16, 64, "%d px")) {
                SaveSettings();
            }
            if (DrawColourPickerButton("Font fill colour", "##fps-fill-picker",
                                       g_fps_fill_colour, setting_width)) {
                SaveSettings();
            }
            if (DrawColourPickerButton("Font outline colour",
                                       "##fps-outline-picker",
                                       g_fps_outline_colour, setting_width)) {
                SaveSettings();
            }
        }
        ImGui::Spacing();
        ImGui::SeparatorText("CRT display filter");
        bool crt_enabled = g_crt_enabled;
        if (ImGui::Checkbox("Enable CRT overlay", &crt_enabled)) {
            g_crt_enabled = crt_enabled;
            SaveSettings();
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped(
            "Applied only to the game image. DKR-R's settings and performance "
            "overlays always remain clear and readable above it.");
        ImGui::PopStyleColor();
        if (g_crt_enabled) {
            ImGui::TextUnformatted("Filter image");
            ImGui::SetNextItemWidth(setting_width);
            if (!g_crt_filters.empty() &&
                ControlCombo("##crt-filter", &g_crt_filter_index,
                             CrtFilterGetter, &g_crt_filters,
                             static_cast<int>(g_crt_filters.size()), 10)) {
                SaveSettings();
                changed = true;
            }
            ImGui::TextUnformatted("Scaling");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlCombo("##crt-scaling", &g_crt_scale_mode,
                             "Stretch to viewport\0Tile at native size\0")) {
                SaveSettings();
                changed = true;
            }
            int density = static_cast<int>(
                std::round(std::clamp(g_crt_strength, 0.0F, 1.0F) * 100.0F));
            ImGui::TextUnformatted("Filter density");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlSliderInt("##crt-density", &density, 0, 100, "%d %%")) {
                g_crt_strength = density / 100.0F;
                SaveSettings();
                changed = true;
            }
        }
        if (ImGui::Button("IMPORT CUSTOM CRT FILTER", {setting_width, 42.0F})) {
            ImportCrtFilterWithDialog();
            changed = true;
        }
        if (!g_crt_status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("%s", g_crt_status.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Custom texture packs");
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped(
            "Native RT64 and Rice PNG packs can be toggled while racing. Rice "
            "archives are converted into a managed RT64 cache, including exact "
            "RGB/alpha reconstruction. Jabo packs are unsupported.");
        ImGui::PopStyleColor();
        const float texture_button_gap = ImGui::GetStyle().ItemSpacing.x;
        const float texture_button_width =
            std::max(120.0F, (setting_width - texture_button_gap) * 0.5F);
        if (ImGui::Button("IMPORT TEXTURE PACK",
                          {texture_button_width, 42.0F})) {
            ImportTexturePackWithDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("REFRESH PACKS", {texture_button_width, 42.0F})) {
            dkr::runtime::texture_packs::refresh();
        }
        const auto texture_packs = dkr::runtime::texture_packs::snapshot(true);
        const std::size_t hidden_pack_count = static_cast<std::size_t>(
            std::count_if(texture_packs.begin(), texture_packs.end(),
                [](const auto& pack) { return pack.hidden; }));
        if (hidden_pack_count > 0) {
            if (ImGui::Checkbox("SHOW HIDDEN PACKS", &g_show_hidden_texture_packs)) {
                changed = true;
            }
        } else {
            g_show_hidden_texture_packs = false;
        }
        const std::size_t shown_pack_count = static_cast<std::size_t>(
            std::count_if(texture_packs.begin(), texture_packs.end(),
                [](const auto& pack) {
                    return !pack.hidden || g_show_hidden_texture_packs;
                }));
        if (shown_pack_count == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped(hidden_pack_count > 0
                ? "All imported texture packs are hidden. Enable SHOW HIDDEN PACKS to manage them."
                : "No texture packs imported yet.");
            ImGui::PopStyleColor();
        }
        bool request_remove_modal = false;
        for (const auto& pack : texture_packs) {
            if (pack.hidden && !g_show_hidden_texture_packs) continue;
            ImGui::PushID(pack.id.c_str());
            const float texture_card_width = available_width;
            const float detail_width =
                std::max(100.0F, texture_card_width - 36.0F);
            const float detail_height = ImGui::CalcTextSize(
                pack.detail.c_str(), nullptr, false, detail_width).y;
            const float card_height = 124.0F + detail_height;
            if (BeginPaddedChild("texture-pack-card",
                                 {texture_card_width, card_height},
                                 true, ImGuiWindowFlags_NoScrollbar)) {
                if (pack.hidden) {
                    ImGui::TextUnformatted(pack.name.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
                    ImGui::TextUnformatted("HIDDEN - MANAGED FILES RETAINED");
                    ImGui::PopStyleColor();
                } else {
                    bool enabled = pack.enabled;
                    ImGui::BeginDisabled(!pack.compatible);
                    if (ImGui::Checkbox(pack.name.c_str(), &enabled)) {
                        dkr::runtime::texture_packs::set_enabled(pack.id, enabled);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PushStyleColor(ImGuiCol_Text,
                    pack.compatible ? kAccent : kWarm);
                const std::string format_label =
                    std::string(dkr::runtime::texture_packs::format_name(pack.format)) +
                    (pack.format == dkr::runtime::texture_packs::Format::LegacyJabo
                        ? " - UNSUPPORTED" : "");
                ImGui::TextUnformatted(format_label.c_str());
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                ImGui::TextWrapped("%s", pack.detail.c_str());
                ImGui::PopStyleColor();
                const float action_gap = ImGui::GetStyle().ItemSpacing.x;
                const float action_width = std::max(
                    (ImGui::GetContentRegionAvail().x - action_gap) * 0.5F, 1.0F);
                if (pack.hidden) {
                    if (ImGui::Button("RESTORE TO LIST", {action_width, 42.0F})) {
                        dkr::runtime::texture_packs::set_hidden(
                            pack.id, false, g_texture_pack_status);
                        changed = true;
                    }
                    ImGui::SameLine(0.0F, action_gap);
                    if (ImGui::Button("DELETE PACK...", {action_width, 42.0F})) {
                        g_texture_pack_remove_id = pack.id;
                        g_texture_pack_remove_name = pack.name;
                        request_remove_modal = true;
                    }
                } else if (ImGui::Button("REMOVE PACK...",
                                         {ImGui::GetContentRegionAvail().x, 42.0F})) {
                    g_texture_pack_remove_id = pack.id;
                    g_texture_pack_remove_name = pack.name;
                    request_remove_modal = true;
                }
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
        if (request_remove_modal) {
            ImGui::OpenPopup("Remove texture pack?");
        }
        ImGui::SetNextWindowSizeConstraints({480.0F, 0.0F}, {680.0F, FLT_MAX});
        if (BeginPaddedModal("Remove texture pack?",
                             ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("REMOVE TEXTURE PACK?");
            ImGui::Separator();
            ImGui::TextWrapped("%s", g_texture_pack_remove_name.c_str());
            ImGui::Dummy({0.0F, 6.0F});
            ImGui::TextWrapped(
                "HIDE FROM LIST keeps DKR-R's managed copy on disk. You can restore it later with SHOW HIDDEN PACKS.");
            ImGui::Dummy({0.0F, 8.0F});
            ImGui::PushStyleColor(ImGuiCol_Text, kRaceRed);
            ImGui::TextUnformatted("WARNING - PERMANENT DELETION CANNOT BE UNDONE");
            ImGui::PopStyleColor();
            ImGui::TextWrapped(
                "DELETE COMPLETELY permanently removes DKR-R's managed archive or converted texture cache. Your original source archive outside DKR-R is never touched.");
            ImGui::Dummy({0.0F, 12.0F});
            if (ImGui::Button("CANCEL", {130.0F, 44.0F})) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("HIDE FROM LIST", {190.0F, 44.0F})) {
                dkr::runtime::texture_packs::set_hidden(
                    g_texture_pack_remove_id, true, g_texture_pack_status);
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.45F, 0.09F, 0.10F, 1.0F});
            if (ImGui::Button("DELETE COMPLETELY", {210.0F, 44.0F})) {
                dkr::runtime::texture_packs::delete_managed(
                    g_texture_pack_remove_id, g_texture_pack_status);
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        if (!g_texture_pack_status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("%s", g_texture_pack_status.c_str());
            ImGui::PopStyleColor();
        }
        const std::string texture_status =
            dkr::runtime::texture_packs::status();
        if (!texture_status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("%s", texture_status.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Camera and scenery");
        bool maximum_detail =
            dkr::runtime::enhancements::maximum_detail_requested();
        if (ImGui::Checkbox("Maximum vehicle detail", &maximum_detail)) {
            dkr::runtime::enhancements::set_maximum_detail_enabled(maximum_detail);
            SaveSettings();
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("KEEPS RACERS ON THEIR HIGHEST LOD MODEL");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        int fov_offset = dkr::runtime::enhancements::fov_offset();
        ImGui::TextUnformatted("Gameplay field-of-view offset");
        ImGui::SetNextItemWidth(setting_width);
        if (ControlSliderInt("##modern-fov-offset", &fov_offset,
                             dkr::runtime::enhancements::kMinimumFovOffset,
                             dkr::runtime::enhancements::kMaximumFovOffset,
                             "%+d degrees", ImGuiSliderFlags_AlwaysClamp)) {
            dkr::runtime::enhancements::set_fov_offset(fov_offset);
            SaveSettings();
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("Adjusts each gameplay level from its authored camera value. Menus, character select and cutscenes keep their original framing.");
        ImGui::PopStyleColor();

        int view_distance =
            dkr::runtime::enhancements::view_distance_multiplier();
        ImGui::TextUnformatted("Scenery and object view distance");
        ImGui::SetNextItemWidth(setting_width);
        if (ControlSliderInt("##modern-view-distance", &view_distance,
                             dkr::runtime::enhancements::kMinimumViewDistanceMultiplier,
                             dkr::runtime::enhancements::kMaximumViewDistanceMultiplier,
                             "%dx", ImGuiSliderFlags_AlwaysClamp)) {
            dkr::runtime::enhancements::set_view_distance_multiplier(view_distance);
            SaveSettings();
            changed = true;
        }
        bool keep_hub_scenery =
            dkr::runtime::enhancements::keep_hub_scenery_requested();
        if (ImGui::Checkbox("Keep all scenery rendered", &keep_hub_scenery)) {
            dkr::runtime::enhancements::set_keep_hub_scenery_enabled(
                keep_hub_scenery);
            SaveSettings();
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("This can increase CPU and GPU load on handheld systems.");
        ImGui::PopStyleColor();

        bool extended_culling =
            dkr::runtime::enhancements::extended_culling_requested();
        if (ImGui::Checkbox("Ultrawide scenery guard", &extended_culling)) {
            dkr::runtime::enhancements::set_extended_culling_enabled(
                extended_culling);
            SaveSettings();
            changed = true;
        }
        if (extended_culling) {
            int guard = dkr::runtime::enhancements::frustum_guard_percent();
            ImGui::TextUnformatted("Culling safety margin");
            ImGui::SetNextItemWidth(setting_width);
            if (ControlSliderInt("##modern-frustum-guard", &guard, 0, 20,
                                 "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
                dkr::runtime::enhancements::set_frustum_guard_percent(guard);
                SaveSettings();
                changed = true;
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("Expands DKR's original CPU visibility planes to the active viewport and adds a small guard band. Objects directly behind the camera still cull normally.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy({0.0F, 16.0F});
    ImGui::BeginDisabled(!modern_profile);
    if (ImGui::Button("RESTORE ACCURATE DEFAULTS", {setting_width, 46.0F})) {
        RememberModernGraphics(config);
        dkr::runtime::enhancements::set_presentation_profile(
            dkr::runtime::enhancements::PresentationProfile::Accurate);
        ApplyProfileGraphics(
            config, dkr::runtime::enhancements::PresentationProfile::Accurate);
        ultramodern::renderer::set_graphics_config(config);
        SaveSettings();
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

void DrawFpsOverlay(RT64::Application&) {
    if (!g_fps_overlay_enabled ||
        !dkr::runtime::enhancements::modern_presentation_enabled()) {
        return;
    }
    const auto measured = dkr::runtime::telemetry::metrics();
    std::vector<std::string> fields;
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%.0f FPS", measured.presented_fps);
    fields.emplace_back(buffer);

    bool frame_time = false;
    bool simulation = false;
    bool graphics = false;
    bool vi = false;
    bool interpolation = false;
    bool audio = false;
    bool target = false;
    bool resolution = false;
    if (g_fps_overlay_detail == 1) {
        frame_time = simulation = target = true;
    } else if (g_fps_overlay_detail == 2) {
        frame_time = simulation = graphics = vi = interpolation = audio =
            target = resolution = true;
    } else if (g_fps_overlay_detail == 3) {
        frame_time = g_fps_custom_frame_time;
        simulation = g_fps_custom_simulation;
        graphics = g_fps_custom_graphics;
        vi = g_fps_custom_vi;
        interpolation = g_fps_custom_interpolation;
        audio = g_fps_custom_audio;
        target = g_fps_custom_target;
        resolution = g_fps_custom_resolution;
    }
    const auto add_rate = [&](bool enabled, const char* format, double value) {
        if (!enabled) return;
        std::snprintf(buffer, sizeof(buffer), format, value);
        fields.emplace_back(buffer);
    };
    add_rate(frame_time, "%.2f MS", measured.frame_time_ms);
    add_rate(simulation, "SIM %.1f HZ", measured.simulation_hz);
    add_rate(graphics, "GFX %.1f HZ", measured.graphics_hz);
    add_rate(vi, "VI %.1f HZ", measured.vi_hz);
    add_rate(interpolation, "INTERP %.1f HZ", measured.interpolated_hz);
    add_rate(audio, "AUDIO %.0f HZ", measured.audio_frames_per_second);
    if (target) {
        const auto& config = ultramodern::renderer::get_graphics_config();
        const int rate = config.rr_option == RefreshRate::Manual
            ? config.rr_manual_value
            : static_cast<int>(ultramodern::get_display_refresh_rate());
        std::snprintf(buffer, sizeof(buffer), "TARGET %d FPS", rate);
        fields.emplace_back(buffer);
    }
    if (resolution) {
        int width = 0;
        int height = 0;
        if (auto* window = static_cast<SDL_Window*>(
                dkr::runtime::platform::sdl_window()); window != nullptr) {
            SDL_GetWindowSize(window, &width, &height);
        }
        std::snprintf(buffer, sizeof(buffer), "%d X %d", width, height);
        fields.emplace_back(buffer);
    }
    if (g_fps_overlay_single_row && fields.size() > 1U) {
        std::string row = fields.front();
        for (std::size_t index = 1; index < fields.size(); ++index) {
            row += "  |  ";
            row += fields[index];
        }
        fields.assign(1U, std::move(row));
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    constexpr float margin = 18.0F;
    ImVec2 position{margin, margin};
    ImVec2 pivot{0.0F, 0.0F};
    if (g_fps_overlay_position == 1 || g_fps_overlay_position == 3) {
        position.x = display.x - margin;
        pivot.x = 1.0F;
    }
    if (g_fps_overlay_position >= 2) {
        position.y = display.y - margin;
        pivot.y = 1.0F;
    }
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.70F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0F, 10.0F});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.025F, 0.08F, 0.12F, 0.82F});
    ImGui::PushStyleColor(ImGuiCol_Border, kWarm);
    if (ImGui::Begin("##dkr-r-fps-overlay", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImFont* font = g_font_fps != nullptr ? g_font_fps : ImGui::GetFont();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(g_fps_fill_colour);
        const ImU32 outline =
            ImGui::ColorConvertFloat4ToU32(g_fps_outline_colour);
        const float font_size = static_cast<float>(
            std::clamp(g_fps_font_size, 16, 64));
        const float edge = std::clamp(font_size * 0.0625F, 1.0F, 4.0F);
        for (const std::string& field : fields) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 extent = font->CalcTextSizeA(
                font_size, FLT_MAX, 0.0F, field.c_str());
            draw->AddText(font, font_size, {at.x - edge, at.y}, outline,
                          field.c_str());
            draw->AddText(font, font_size, {at.x + edge, at.y}, outline,
                          field.c_str());
            draw->AddText(font, font_size, {at.x, at.y - edge}, outline,
                          field.c_str());
            draw->AddText(font, font_size, {at.x, at.y + edge}, outline,
                          field.c_str());
            draw->AddText(font, font_size, at, fill, field.c_str());
            ImGui::Dummy({extent.x, extent.y + 2.0F});
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void DrawSaveNameEditor(const char* label, std::string& value, float width) {
    using dkr::runtime::saves::codec::sanitise_name;
    constexpr std::array<const char*, 29> choices{{
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
        ".", "?", "SPACE"}};
    std::string padded = sanitise_name(value);
    padded.resize(3U, ' ');
    ImGui::TextUnformatted(label);
    if (ImGui::BeginTable("name-characters", 3,
                          ImGuiTableFlags_SizingStretchSame, {width, 0.0F})) {
        for (int character = 0; character < 3; ++character) {
            ImGui::TableNextColumn();
            ImGui::PushID(character);
            int selected = 28;
            if (padded[character] >= 'A' && padded[character] <= 'Z') {
                selected = padded[character] - 'A';
            } else if (padded[character] == '.') {
                selected = 26;
            } else if (padded[character] == '?') {
                selected = 27;
            }
            ImGui::SetNextItemWidth(-1.0F);
            if (ControlCombo("##character", &selected,
                             [](void* data, int index, const char** output) {
                                 const auto* items = static_cast<
                                     const std::array<const char*, 29>*>(data);
                                 if (index < 0 || index >= static_cast<int>(items->size())) {
                                     return false;
                                 }
                                 *output = (*items)[static_cast<std::size_t>(index)];
                                 return true;
                             }, const_cast<void*>(static_cast<const void*>(&choices)),
                             static_cast<int>(choices.size()))) {
                padded[character] = selected < 26 ? static_cast<char>('A' + selected)
                    : selected == 26 ? '.' : selected == 27 ? '?' : ' ';
                value = sanitise_name(padded);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

bool DrawLabeledSliderInt(const char* label, const char* id, int* value,
                          int minimum, int maximum, const char* format,
                          float width) {
    ImGui::TextWrapped("%s", label);
    ImGui::SetNextItemWidth(width);
    return ControlSliderInt(id, value, minimum, maximum, format,
                            ImGuiSliderFlags_AlwaysClamp);
}

bool DrawWrappedCheckbox(const char* label, const char* id, bool* value) {
    const bool changed = ImGui::Checkbox(id, value);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", label);
    return changed;
}

float ScrollbarSafeControlWidth(float requested_width) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().x;
    // Save Builder pages are nested inside a scrolling card. Leave a full
    // scrollbar gutter plus some breathing room on both sides so combo boxes
    // and their popups never sit underneath the bar at narrow window sizes.
    // The cap also keeps long controls readable on ultrawide launchers.
    constexpr float kComfortableSaveControlWidth = 620.0F;
    const float scrollbar_gutter = style.ScrollbarSize +
        (style.ItemSpacing.x * 2.0F);
    return std::max(std::min({requested_width, available,
                              kComfortableSaveControlWidth}) -
                        scrollbar_gutter,
                    1.0F);
}

void DrawMagicCodes(float width) {
    using namespace dkr::runtime::magic_codes;

    const float control_width = ScrollbarSafeControlWidth(width);
    static bool expanded = false;
    if (ImGui::Button(expanded ? "MAGIC CODES  -  CLOSE"
                               : "MAGIC CODES  -  OPEN",
                      {control_width, 42.0F})) {
        expanded = !expanded;
    }
    if (!expanded) {
        return;
    }
    bool diagnostics_heading_drawn = false;
    for (const auto& definition : kMagicCodeDefinitions) {
        if (definition.diagnostic && !diagnostics_heading_drawn) {
            ImGui::SeparatorText("Diagnostics - use with care");
            diagnostics_heading_drawn = true;
        }
        ImGui::PushID(static_cast<int>(definition.internal_index));
        bool enabled = magic_code_enabled(selected_mask(),
                                          definition.internal_index);
        const std::string label = std::string(definition.phrase) +
            (definition.one_shot ? " - NEXT LAUNCH" : "");
        if (ImGui::Checkbox(label.c_str(), &enabled)) {
            std::string error;
            if (set_enabled(definition.internal_index, enabled, error)) {
                SaveSettings();
                g_save_manager_status = enabled
                    ? std::string(definition.phrase) +
                        (definition.one_shot
                            ? " queued for the next game launch."
                            : " will be active on launch.")
                    : std::string(definition.phrase) + " disabled.";
            } else {
                g_save_manager_status = error;
            }
        }
        ImGui::Indent(28.0F);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                              std::max(width - 28.0F, 1.0F));
        if (definition.diagnostic) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRaceRed);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        }
        ImGui::TextWrapped("%s", definition.effect);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Unindent(28.0F);
        ImGui::PopID();
    }

    ImGui::Dummy({0.0F, 6.0F});
    if (ImGui::Button("CLEAR ALL MAGIC CODES", {control_width, 42.0F})) {
        std::string error;
        if (clear_all(error)) {
            SaveSettings();
            g_save_manager_status = "All launch Magic Codes cleared.";
        } else {
            g_save_manager_status = error;
        }
    }
}

std::string FormatRecordTime(std::uint16_t frames) {
    if (frames == 0U) {
        return "No personal record yet";
    }
    const unsigned minutes = frames / (60U * 60U);
    const unsigned remainder = frames % (60U * 60U);
    const unsigned seconds = remainder / 60U;
    const unsigned hundredths = ((remainder % 60U) * 100U) / 60U;
    char result[32]{};
    std::snprintf(result, sizeof(result), "%02u:%02u:%02u",
                  minutes, seconds, hundredths);
    return result;
}

void DrawAdventureBuilder(float width) {
    using namespace dkr::runtime::saves;
    using namespace dkr::runtime::saves::codec;

    ImGui::TextWrapped("Edit DKR's native EEPROM fields. Applying always creates a dated safety backup, rebuilds every checksum, validates a temporary image and atomically swaps it into T.T.'s garage.");
    if (ImGui::Button(g_save_builder_image ? "RELOAD LIVE SAVE" : "OPEN SAVE BUILDER",
                      {width, 46.0F})) {
        SaveImage image{};
        std::string error;
        if (load_adventure(image, error)) {
            normalise_editable_fields(image);
            g_save_builder_image = std::move(image);
            g_save_manager_status = "Adventure EEPROM loaded into the builder.";
        } else {
            g_save_manager_status = error;
        }
    }
    if (!g_save_builder_image) {
        return;
    }

    SaveImage& image = *g_save_builder_image;
    ImGui::Dummy({0.0F, 8.0F});
    if (ImGui::BeginTabBar("save-builder-sections",
                           ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int slot_index = 0; slot_index < static_cast<int>(kAdventureSlotCount);
             ++slot_index) {
            const std::string label = "ADVENTURE " + std::to_string(slot_index + 1);
            if (ImGui::BeginTabItem(label.c_str())) {
                g_save_builder_slot = slot_index;
                AdventureSlot& slot = image.slots[static_cast<std::size_t>(slot_index)];
                ImGui::PushID(slot_index);
                const float control_width = ScrollbarSafeControlWidth(width);
                DrawSaveNameEditor("Racer initials", slot.name, control_width);

                ImGui::SeparatorText("Golden Balloons");
                ImGui::TextWrapped("The total is calculated automatically. DKR-R's central island region contains seven balloons; each racing world contains eight.");
                unsigned racing_world_total = 0U;
                for (std::size_t world = 1U; world < kWorldCount; ++world) {
                    racing_world_total += slot.balloons[world];
                }
                int hub_balloons = slot.balloons[0] > racing_world_total
                    ? static_cast<int>(std::min<unsigned>(
                          slot.balloons[0] - racing_world_total,
                          kMaximumHubBalloons))
                    : 0;
                char total_label[24]{};
                std::snprintf(total_label, sizeof(total_label), "%u / %u",
                              slot.balloons[0], kMaximumTotalBalloons);
                ImGui::TextUnformatted("Total Golden Balloons (calculated)");
                ImGui::ProgressBar(
                    static_cast<float>(slot.balloons[0]) /
                        static_cast<float>(kMaximumTotalBalloons),
                    {control_width, 0.0F}, total_label);

                constexpr std::array<const char*, kWorldCount> area_names{{
                    "DKR-R", "Dino Domain", "Sherbet Island",
                    "Snowflake Mountain", "Dragon Forest", "Future Fun Land"}};
                for (std::size_t area = 0; area < kWorldCount; ++area) {
                    int value = area == 0U ? hub_balloons : slot.balloons[area];
                    const int maximum = area == 0U
                        ? kMaximumHubBalloons : kMaximumWorldBalloons;
                    ImGui::PushID(static_cast<int>(area));
                    if (DrawLabeledSliderInt(area_names[area], "##balloons",
                                             &value, 0, maximum, "%d",
                                             control_width)) {
                        if (area == 0U) {
                            hub_balloons = value;
                        } else {
                            slot.balloons[area] =
                                static_cast<std::uint8_t>(value);
                        }
                        unsigned new_total = static_cast<unsigned>(hub_balloons);
                        for (std::size_t world = 1U; world < kWorldCount; ++world) {
                            new_total += slot.balloons[world];
                        }
                        slot.balloons[0] = static_cast<std::uint8_t>(
                            std::min<unsigned>(new_total, kMaximumTotalBalloons));
                    }
                    ImGui::PopID();
                }

                ImGui::SeparatorText("Amulets and keys");
                int tt_amulet = slot.tt_amulet;
                int wizpig_amulet = slot.wizpig_amulet;
                if (DrawLabeledSliderInt("T.T. amulet pieces", "##tt-amulet",
                                         &tt_amulet, 0, kMaximumAmuletPieces,
                                         "%d", control_width)) {
                    slot.tt_amulet = static_cast<std::uint8_t>(tt_amulet);
                }
                if (DrawLabeledSliderInt("Wizpig amulet pieces",
                                         "##wizpig-amulet", &wizpig_amulet,
                                         0, kMaximumAmuletPieces, "%d", control_width)) {
                    slot.wizpig_amulet = static_cast<std::uint8_t>(wizpig_amulet);
                }
                constexpr std::array<const char*, 4> key_names{{
                    "Dino Domain key", "Snowflake Mountain key",
                    "Sherbet Island key", "Dragon Forest key"}};
                constexpr std::array<unsigned, 4> key_bits{{1U, 2U, 3U, 4U}};
                for (std::size_t key = 0; key < key_bits.size(); ++key) {
                    const auto mask = static_cast<std::uint8_t>(1U << key_bits[key]);
                    bool value = (slot.keys & mask) != 0U;
                    const char* key_label = key_names[key];
                    ImGui::PushID(static_cast<int>(key));
                    if (DrawWrappedCheckbox(key_label, "##key", &value)) {
                        if (value) slot.keys |= mask;
                        else slot.keys &= static_cast<std::uint8_t>(~mask);
                    }
                    ImGui::PopID();
                }

                static std::array<bool, kAdventureSlotCount>
                    course_progress_expanded{{true, true, true}};
                bool& progress_expanded = course_progress_expanded[
                    static_cast<std::size_t>(slot_index)];
                if (ImGui::Button(progress_expanded
                                      ? "COURSE PROGRESS  -  CLOSE"
                                      : "COURSE PROGRESS  -  OPEN",
                                  {control_width, 42.0F})) {
                    progress_expanded = !progress_expanded;
                }
                if (progress_expanded) {
                    constexpr const char* statuses =
                        "Not started\0Race won\0Silver Coins won\0Complete\0";
                    const auto& names = course_names();
                    for (std::size_t course = 0; course < kCourseCount; ++course) {
                        ImGui::PushID(static_cast<int>(course));
                        int status = slot.course_status[course];
                        ImGui::TextUnformatted(names[course]);
                        ImGui::SetNextItemWidth(control_width);
                        if (ControlCombo("##course-status", &status, statuses)) {
                            slot.course_status[course] = static_cast<std::uint8_t>(status);
                        }
                        ImGui::PopID();
                    }
                }

                if (ImGui::Button("MAX OUT THIS ADVENTURE",
                                  {control_width, 42.0F})) {
                    std::fill(slot.course_status.begin(), slot.course_status.end(), 3U);
                    slot.taj_flags = 0x3F;
                    slot.trophies = 0x3FF;
                    slot.bosses = 0xFFF;
                    slot.balloons = {47, 8, 8, 8, 8, 8};
                    slot.tt_amulet = 4;
                    slot.wizpig_amulet = 4;
                    slot.world_flags.fill(0xFFFF);
                    slot.keys = 0x1E;
                }
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }

        if (ImGui::BeginTabItem("UNLOCKS")) {
            constexpr std::array<const char*, 20> tt_trial_names{{
                "Ancient Lake", "Fossil Canyon", "Jungle Falls",
                "Hot Top Volcano", "Whale Bay", "Crescent Island",
                "Pirate Lagoon", "Treasure Caves", "Everfrost Peak",
                "Walrus Cove", "Snowball Valley", "Frosty Village",
                "Boulder Canyon", "Greenwood Village", "Windmill Plains",
                "Haunted Woods", "Spacedust Alley", "Darkmoon Caverns",
                "Star City", "Spaceport Alpha"}};
            ImGui::Checkbox("Adventure Two", &image.settings.adventure_two);
            ImGui::Checkbox("Drumstick", &image.settings.drumstick);
            ImGui::Checkbox("Subtitles", &image.settings.subtitles);
            int language = image.settings.language;
            ImGui::TextUnformatted("Language");
            ImGui::SetNextItemWidth(width);
            if (ControlCombo("##save-language", &language,
                             "English\0German\0French\0Japanese\0")) {
                image.settings.language = static_cast<std::uint8_t>(language);
            }
            ImGui::SeparatorText("T.T. time-trial victories");
            for (std::size_t trial = 0; trial < image.settings.tt_trials.size(); ++trial) {
                ImGui::PushID(static_cast<int>(trial));
                ImGui::Checkbox(tt_trial_names[trial], &image.settings.tt_trials[trial]);
                ImGui::PopID();
            }
            if (ImGui::Button("UNLOCK ALL RACERS AND MODES", {width, 42.0F})) {
                image.settings.adventure_two = true;
                image.settings.drumstick = true;
                image.settings.tt_trials.fill(true);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("T.T. RECORDS")) {
            ImGui::TextWrapped("Personal records are view-only. Race in Time Trial mode to set or improve them.");
            const auto draw_records = [&](const char* heading,
                                          const std::array<Record, kRecordCount>& records) {
                if (!ImGui::CollapsingHeader(heading)) return;
                for (std::size_t index = 0; index < records.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", record_names()[index]);
                    const std::string time = FormatRecordTime(records[index].time);
                    ImGui::TextDisabled("Time: %s", time.c_str());
                    if (records[index].time != 0U) {
                        const std::string initials = records[index].initials.empty()
                            ? "---" : records[index].initials;
                        ImGui::TextDisabled("Racer: %s", initials.c_str());
                    }
                    ImGui::PopID();
                }
            };
            draw_records("Fastest laps", image.fastest_laps);
            draw_records("Course times", image.course_times);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Dummy({0.0F, 12.0F});
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    if (ImGui::Button("BACK UP AND APPLY CHECKSUM-SAFE SAVE", {width, 52.0F})) {
        ImGui::OpenPopup("Apply Save Builder changes?");
    }
    ImGui::PopStyleColor();
    if (BeginPaddedModal("Apply Save Builder changes?",
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Replace the live Adventure EEPROM after creating a dated backup?");
        if (ImGui::Button("CANCEL", {140.0F, 42.0F})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("BACK UP AND APPLY", {220.0F, 42.0F})) {
            std::string error;
            if (commit_adventure(image, error)) {
                g_save_manager_status = "Save Builder changes applied. Checksums verified and the previous EEPROM is backed up.";
            } else {
                g_save_manager_status = error;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawSaveManager(bool live = false) {
    const auto info = dkr::runtime::saves::adventure_info();
    const float width = std::max(ImGui::GetContentRegionAvail().x - 30.0F, 1.0F);
    ImGui::Dummy({0.0F, 4.0F});
    if (live) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.045F, 0.18F, 0.25F, 0.96F});
        BeginPaddedChild("live-save-manager-lock", {width, 148.0F}, true,
                         ImGuiWindowFlags_NoScrollbar, {20.0F, 18.0F});
        ImGui::PushTextWrapPos(width - 18.0F);
        ImGui::TextUnformatted("PIT LANE SAFETY LOCK");
        ImGui::TextWrapped("Save import, restore and reset are available before the game starts. Close the game and use Save Manager so DKR cannot write to the same EEPROM or Controller Pak during a transfer.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.045F, 0.18F, 0.25F, 0.96F});
    BeginPaddedChild("adventure-save-card", {width, 206.0F}, true,
                     ImGuiWindowFlags_NoScrollbar, {20.0F, 18.0F});
    ImGui::PushTextWrapPos(width - 18.0F);
    ImGui::TextUnformatted("ADVENTURE PROGRESS");
    if (!info.exists) {
        ImGui::TextDisabled("No Adventure save yet. DKR will create one after your first save.");
    } else if (info.size != dkr::runtime::saves::codec::kImageSize) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRaceRed);
        ImGui::TextWrapped("This file is %llu bytes; DKR Adventure EEPROMs must be exactly 512 bytes. Import a known-good backup before racing.",
                           static_cast<unsigned long long>(info.size));
        ImGui::PopStyleColor();
    } else if (!info.valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRaceRed);
        ImGui::TextWrapped("This 512-byte EEPROM has invalid DKR checksums. Import a known-good backup before editing it.");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted("READY - 512 BYTE EEPROM");
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("%s", PathUtf8(info.path).c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 10.0F});

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const bool row = width >= 620.0F;
    const float button_width = row ? (width - gap * 2.0F) / 3.0F : width;
    ImGui::BeginDisabled(!info.valid);
    if (ImGui::Button("MAKE SAFETY BACKUP", {button_width, 46.0F})) {
        std::filesystem::path created;
        std::string error;
        if (dkr::runtime::saves::backup_adventure(created, error)) {
            g_save_manager_status = "Safety backup parked in T.T.'s garage.";
        } else {
            g_save_manager_status = error;
        }
    }
    if (row) ImGui::SameLine();
    if (ImGui::Button("EXPORT SAVE", {button_width, 46.0F})) {
        ExportAdventureWithDialog();
    }
    ImGui::EndDisabled();
    if (row) ImGui::SameLine();
    if (ImGui::Button("IMPORT SAVE", {button_width, 46.0F})) {
        ImportAdventureWithDialog();
    }

    ImGui::Dummy({0.0F, 18.0F});
    DrawAdventureBuilder(width);

    ImGui::Dummy({0.0F, 18.0F});
    DrawMagicCodes(width);

    ImGui::Dummy({0.0F, 14.0F});
    ImGui::SeparatorText("Complete garage transfer");
    ImGui::TextWrapped("A single path-free bundle carries the Adventure EEPROM and every present virtual Controller Pak between Windows and Steam Deck.");
    const float bundle_button_width = row ? (width - gap) * 0.5F : width;
    if (ImGui::Button("EXPORT COMPLETE GARAGE", {bundle_button_width, 46.0F})) {
        ExportSaveBundleWithDialog();
    }
    if (row) ImGui::SameLine();
    if (ImGui::Button("IMPORT COMPLETE GARAGE", {bundle_button_width, 46.0F})) {
        ImportSaveBundleWithDialog();
    }

    ImGui::Dummy({0.0F, 14.0F});
    ImGui::SeparatorText("Virtual Controller Paks");
    for (int channel = 0; channel < dkr::runtime::saves::kControllerPakCount;
         ++channel) {
        const auto pak_info = dkr::runtime::saves::controller_pak_info(channel);
        ImGui::PushID(channel);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.045F, 0.18F, 0.25F, 0.96F});
        BeginPaddedChild("controller-pak-card", {width, 94.0F}, true,
                         ImGuiWindowFlags_NoScrollbar, {16.0F, 14.0F});
        ImGui::Text("CONTROLLER %d", channel + 1);
        ImGui::SameLine();
        if (!pak_info.exists) {
            ImGui::TextDisabled("Not created yet");
        } else if (pak_info.valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted("PAK READY");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kRaceRed);
            ImGui::TextUnformatted("RECOVERY NEEDED");
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorPosX(16.0F);
        ImGui::TextDisabled("%s", PathUtf8(pak_info.path).c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    if (!g_save_manager_status.empty()) {
        ImGui::Dummy({0.0F, 8.0F});
        ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
        ImGui::TextWrapped("%s", g_save_manager_status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy({0.0F, 18.0F});
    ImGui::SeparatorText("Recent automatic backups");
    const auto backups = dkr::runtime::saves::adventure_backups();
    if (backups.empty()) {
        ImGui::TextDisabled("No backups are parked here yet.");
    } else {
        const std::size_t shown = std::min<std::size_t>(backups.size(), 6U);
        for (std::size_t index = 0; index < shown; ++index) {
            ImGui::PushID(static_cast<int>(index));
            const std::string filename = PathUtf8(backups[index].filename());
            const float restore_width = std::min(190.0F, width * 0.33F);
            if (ImGui::BeginTable("backup-row", 2,
                    ImGuiTableFlags_SizingStretchProp, {width, 0.0F})) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("restore", ImGuiTableColumnFlags_WidthFixed,
                                        restore_width);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextWrapped("%s", filename.c_str());
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Button("RESTORE", {-1.0F, 38.0F})) {
                    std::string error;
                    if (dkr::runtime::saves::import_adventure(backups[index], error)) {
                        g_save_builder_image.reset();
                        g_save_manager_status = "Backup restored. The replaced save was backed up too.";
                    } else {
                        g_save_manager_status = error;
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy({0.0F, 18.0F});
    ImGui::PushStyleColor(ImGuiCol_Button, {0.45F, 0.09F, 0.10F, 1.0F});
    if (ImGui::Button("START A FRESH ADVENTURE", {width, 44.0F})) {
        ImGui::OpenPopup("Reset Adventure save?");
    }
    ImGui::PopStyleColor();
    if (BeginPaddedModal("Reset Adventure save?",
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Park the current save in a backup, then start fresh?");
        ImGui::TextDisabled("The backup can be restored from this screen later.");
        if (ImGui::Button("CANCEL", {130.0F, 40.0F})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.45F, 0.09F, 0.10F, 1.0F});
        if (ImGui::Button("START FRESH", {150.0F, 40.0F})) {
            std::string error;
            if (dkr::runtime::saves::reset_adventure(error)) {
                g_save_builder_image.reset();
                g_save_manager_status = "Fresh Adventure save created; the previous journey is safe in backups.";
            } else {
                g_save_manager_status = error;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
}

void DrawAudioSettings(float width) {
    const float control_width = std::min(width, 720.0F);
    float master = dkr::runtime::platform::master_volume() * 100.0F;
    ImGui::TextUnformatted("Master volume");
    ImGui::SetNextItemWidth(control_width);
    if (ControlSliderFloat("##audio-master", &master, 0.0F, 100.0F,
                           "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        dkr::runtime::platform::set_master_volume(master / 100.0F);
        SaveSettings();
    }
    if (!dkr::runtime::enhancements::modern_options_visible(
            dkr::runtime::enhancements::presentation_profile())) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.055F, 0.19F, 0.29F, 1.0F});
        BeginPaddedChild("accurate-audio-lock", {control_width, 104.0F}, true,
                         ImGuiWindowFlags_NoScrollbar, {18.0F, 15.0F});
        ImGui::PushTextWrapPos(std::max(control_width - 18.0F, 1.0F));
        ImGui::TextUnformatted("Original island mix - Accurate");
        ImGui::TextWrapped("Music, effects, vehicles and EQ stay at their authored values. Master volume remains available.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Island mix");
    const auto volume_slider = [&](const char* label, const char* id,
                                   float value, auto setter) {
        float percent = value * 100.0F;
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(control_width);
        if (ControlSliderFloat(id, &percent, 0.0F, 100.0F, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp)) {
            setter(percent / 100.0F);
            SaveSettings();
        }
    };
    volume_slider("Music", "##audio-music", dkr::runtime::audio::music_volume(),
                  dkr::runtime::audio::set_music_volume);
    volume_slider("Sound effects", "##audio-effects",
                  dkr::runtime::audio::sound_effects_volume(),
                  dkr::runtime::audio::set_sound_effects_volume);
    volume_slider("Vehicle sounds", "##audio-vehicles",
                  dkr::runtime::audio::vehicle_volume(),
                  dkr::runtime::audio::set_vehicle_volume);
    volume_slider("Nature and ambience", "##audio-nature",
                  dkr::runtime::audio::nature_volume(),
                  dkr::runtime::audio::set_nature_volume);

    ImGui::Spacing();
    ImGui::SeparatorText("Three-band EQ");
    const auto eq_slider = [&](const char* label, const char* id,
                               float value, auto setter) {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(control_width);
        if (ControlSliderFloat(id, &value, -12.0F, 12.0F, "%+.1f dB",
                               ImGuiSliderFlags_AlwaysClamp)) {
            setter(value);
            SaveSettings();
        }
    };
    eq_slider("Bass", "##audio-bass", dkr::runtime::platform::bass_gain(),
              dkr::runtime::platform::set_bass_gain);
    eq_slider("Mid", "##audio-mid", dkr::runtime::platform::mid_gain(),
              dkr::runtime::platform::set_mid_gain);
    eq_slider("Treble", "##audio-treble", dkr::runtime::platform::treble_gain(),
              dkr::runtime::platform::set_treble_gain);
    if (ImGui::Button("RESTORE ORIGINAL MIX", {control_width, 44.0F})) {
        dkr::runtime::audio::set_music_volume(1.0F);
        dkr::runtime::audio::set_sound_effects_volume(1.0F);
        dkr::runtime::audio::set_vehicle_volume(1.0F);
        dkr::runtime::audio::set_nature_volume(1.0F);
        dkr::runtime::platform::set_bass_gain(0.0F);
        dkr::runtime::platform::set_mid_gain(0.0F);
        dkr::runtime::platform::set_treble_gain(0.0F);
        SaveSettings();
    }
}

std::string ShortcutBindingName(CaptureDevice device,
                                dkr::runtime::input::ShortcutBinding binding) {
    const auto source_name = [device](int source) {
        return device == CaptureDevice::Keyboard
            ? dkr::runtime::input::keyboard_binding_name(source)
            : dkr::runtime::input::controller_binding_name(source);
    };
    if (binding.primary == dkr::runtime::input::kUnbound) {
        return "Unbound";
    }
    std::string name = source_name(binding.primary);
    if (binding.secondary != dkr::runtime::input::kUnbound) {
        name += " + ";
        name += source_name(binding.secondary);
    }
    return name;
}

void CommitShortcutCapture() {
    dkr::runtime::input::ShortcutBinding binding{
        g_shortcut_capture_sources[0],
        g_shortcut_capture_count > 1
            ? g_shortcut_capture_sources[1]
            : dkr::runtime::input::kUnbound};
    if (g_capture_device == CaptureDevice::Keyboard) {
        dkr::runtime::input::set_quick_restart_keyboard_binding(binding);
    } else if (g_capture_device == CaptureDevice::Controller) {
        dkr::runtime::input::set_quick_restart_controller_binding(binding);
    }
    SaveSettings();
    g_capture_finished = true;
}

void BeginShortcutCapture(CaptureDevice device) {
    g_capture_action = kShortcutCaptureAction;
    g_capture_device = device;
    g_capture_popup_pending = true;
    g_capture_finished = false;
    g_shortcut_capture_sources = {
        dkr::runtime::input::kUnbound, dkr::runtime::input::kUnbound};
    g_shortcut_capture_count = 0;
    g_shortcut_capture_deadline = {};
}

void DrawControlsReference(bool live) {
    using dkr::runtime::input::Action;
    ImGui::TextUnformatted("DRIVER BINDINGS");
    ImGui::Separator();
    const auto begin_capture_button = [](Action action, std::size_t index,
                                         CaptureDevice device, float width) {
        const std::string binding_name = device == CaptureDevice::Keyboard
            ? dkr::runtime::input::keyboard_binding_name(
                  dkr::runtime::input::keyboard_binding(action))
            : dkr::runtime::input::controller_binding_name(
                  dkr::runtime::input::controller_binding(action));
        ImGui::PushID(static_cast<int>(index * 2U +
                      (device == CaptureDevice::Controller ? 1U : 0U)));
        const bool pressed = ImGui::Button(binding_name.c_str(), {width, 38.0F});
        ImGui::PopID();
        if (pressed) {
            g_capture_action = static_cast<int>(index);
            g_capture_device = device;
            g_capture_popup_pending = true;
            g_capture_finished = false;
        }
    };

    // Keep a real gutter on the right at every width. Tables and full-width
    // capture buttons otherwise consume the parent's last pixel and appear to
    // collide with the card border/scrollbar.
    constexpr float kControlsRightPadding = 30.0F;
    const float available_width = std::max(
        ImGui::GetContentRegionAvail().x - kControlsRightPadding, 1.0F);
    if (available_width >= 620.0F &&
        ImGui::BeginTable("controls", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_SizingStretchProp,
                          {available_width, 0.0F})) {
        const float label_width = std::clamp(available_width * 0.30F, 180.0F, 260.0F);
        ImGui::TableSetupColumn("N64 CONTROL", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("KEYBOARD", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("GAMEPAD", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < dkr::runtime::input::action_count(); ++index) {
            const auto action = static_cast<Action>(index);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", dkr::runtime::input::action_label(action));
            ImGui::TableSetColumnIndex(1);
            begin_capture_button(action, index, CaptureDevice::Keyboard, -1.0F);
            ImGui::TableSetColumnIndex(2);
            begin_capture_button(action, index, CaptureDevice::Controller, -1.0F);
        }
        ImGui::EndTable();
    } else if (available_width < 620.0F) {
        // A two-row card is easier to read and drive with a controller than
        // squeezing the three desktop columns into a narrow overlay.
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const bool side_by_side = available_width >= 360.0F;
        for (std::size_t index = 0; index < dkr::runtime::input::action_count(); ++index) {
            const auto action = static_cast<Action>(index);
            ImGui::PushID(static_cast<int>(index));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.045F, 0.18F, 0.25F, 0.92F});
            const float card_height = side_by_side ? 112.0F : 166.0F;
            BeginPaddedChild("binding-card", {available_width, card_height}, true,
                             ImGuiWindowFlags_NoScrollbar, {16.0F, 14.0F});
            ImGui::PushTextWrapPos(available_width - 16.0F);
            ImGui::TextUnformatted(dkr::runtime::input::action_label(action));
            ImGui::PopTextWrapPos();
            ImGui::SetCursorPosX(16.0F);
            const float inner_width = std::max(available_width - 32.0F, 1.0F);
            const float button_width = side_by_side
                ? std::max((inner_width - gap) * 0.5F, 1.0F)
                : inner_width;
            begin_capture_button(action, index, CaptureDevice::Keyboard, button_width);
            if (side_by_side) {
                ImGui::SameLine(0.0F, gap);
            } else {
                ImGui::SetCursorPosX(16.0F);
            }
            begin_capture_button(action, index, CaptureDevice::Controller, button_width);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("RESTORE T.T.'S DEFAULTS", {available_width, 44.0F})) {
        dkr::runtime::input::reset_defaults();
        dkr::runtime::input::set_stick_deadzone(23.95F);
        dkr::runtime::input::set_stick_anti_deadzone(0.0F);
        dkr::runtime::input::set_stick_sensitivity(100.0F);
        dkr::runtime::input::set_stick_curve(1.0F);
        dkr::runtime::input::set_stick_x_inverted(false);
        dkr::runtime::input::set_stick_y_inverted(false);
        dkr::runtime::input::set_trigger_threshold(0.5F);
        SaveSettings();
    }

    if (dkr::runtime::enhancements::modern_options_visible(
            dkr::runtime::enhancements::presentation_profile())) {
        if (g_capture_action == kShortcutCaptureAction &&
            g_shortcut_capture_count == 1 &&
            std::chrono::steady_clock::now() >= g_shortcut_capture_deadline) {
            CommitShortcutCapture();
        }
        ImGui::Dummy({0.0F, 18.0F});
        ImGui::SeparatorText("Controller feel");
        const auto tune_slider = [&](const char* label, const char* id,
                                     float value, float minimum, float maximum,
                                     const char* format, auto setter) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(available_width);
            if (ControlSliderFloat(id, &value, minimum, maximum, format,
                                   ImGuiSliderFlags_AlwaysClamp)) {
                setter(value);
                SaveSettings();
            }
        };
        tune_slider("Stick deadzone", "##stick-deadzone",
                    dkr::runtime::input::stick_deadzone(), 0.0F, 35.0F, "%.1f%%",
                    dkr::runtime::input::set_stick_deadzone);
        tune_slider("Stick anti-deadzone", "##stick-anti-deadzone",
                    dkr::runtime::input::stick_anti_deadzone(), 0.0F, 50.0F, "%.1f%%",
                    dkr::runtime::input::set_stick_anti_deadzone);
        tune_slider("Stick sensitivity", "##stick-sensitivity",
                    dkr::runtime::input::stick_sensitivity(), 50.0F, 150.0F, "%.0f%%",
                    dkr::runtime::input::set_stick_sensitivity);
        tune_slider("Response curve", "##stick-curve",
                    dkr::runtime::input::stick_curve(), 0.5F, 2.5F, "%.2f",
                    dkr::runtime::input::set_stick_curve);
        tune_slider("Trigger threshold", "##trigger-threshold",
                    dkr::runtime::input::trigger_threshold(), 0.05F, 0.95F, "%.2f",
                    dkr::runtime::input::set_trigger_threshold);
        bool invert_x = dkr::runtime::input::stick_x_inverted();
        if (ImGui::Checkbox("Invert horizontal stick", &invert_x)) {
            dkr::runtime::input::set_stick_x_inverted(invert_x);
            SaveSettings();
        }
        bool invert_y = dkr::runtime::input::stick_y_inverted();
        if (ImGui::Checkbox("Invert vertical stick", &invert_y)) {
            dkr::runtime::input::set_stick_y_inverted(invert_y);
            SaveSettings();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("These adjustments shape the final N64 stick sample once per authored game update. Accurate keeps the original response.");
        ImGui::PopStyleColor();

        ImGui::Dummy({0.0F, 18.0F});
        ImGui::SeparatorText("Quick restart");
        bool quick_restart = dkr::runtime::input::quick_restart_enabled();
        if (ImGui::Checkbox("Enable quick race restart", &quick_restart)) {
            dkr::runtime::input::set_quick_restart_enabled(quick_restart);
            SaveSettings();
        }
        if (quick_restart) {
            const float shortcut_gap = ImGui::GetStyle().ItemSpacing.x;
            const float shortcut_width = std::max(
                (available_width - shortcut_gap) * 0.5F, 1.0F);
            const auto keyboard =
                dkr::runtime::input::quick_restart_keyboard_binding();
            const auto controller =
                dkr::runtime::input::quick_restart_controller_binding();
            ImGui::TextUnformatted("Keyboard shortcut");
            ImGui::SameLine(shortcut_width + shortcut_gap);
            ImGui::TextUnformatted("Controller shortcut");
            if (ImGui::Button(
                    ShortcutBindingName(CaptureDevice::Keyboard, keyboard).c_str(),
                    {shortcut_width, 42.0F})) {
                BeginShortcutCapture(CaptureDevice::Keyboard);
            }
            ImGui::SameLine(0.0F, shortcut_gap);
            if (ImGui::Button(
                    ShortcutBindingName(CaptureDevice::Controller, controller).c_str(),
                    {shortcut_width, 42.0F})) {
                BeginShortcutCapture(CaptureDevice::Controller);
            }
        }

        ImGui::Dummy({0.0F, 18.0F});
        ImGui::SeparatorText("Motion steering");
        bool gyro = dkr::runtime::input::gyro_enabled();
        if (ImGui::Checkbox("Gyro steering", &gyro)) {
            dkr::runtime::input::set_gyro_enabled(gyro);
            SaveSettings();
        }
        if (gyro) {
            int axis = static_cast<int>(dkr::runtime::input::gyro_axis());
            ImGui::TextUnformatted("Motion style");
            ImGui::SetNextItemWidth(available_width);
            if (ControlCombo("##gyro-axis", &axis,
                             "Roll controller like a wheel\0Yaw controller left and right\0")) {
                dkr::runtime::input::set_gyro_axis(
                    axis == 1 ? dkr::runtime::input::GyroAxis::Yaw
                              : dkr::runtime::input::GyroAxis::Roll);
                SaveSettings();
            }
            float sensitivity = dkr::runtime::input::gyro_sensitivity();
            ImGui::TextUnformatted("Horizontal gyro sensitivity");
            ImGui::SetNextItemWidth(available_width);
            if (ControlSliderFloat("##gyro-x-sensitivity", &sensitivity,
                                   25.0F, 300.0F, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                dkr::runtime::input::set_gyro_sensitivity(sensitivity);
                SaveSettings();
            }
            float y_sensitivity = dkr::runtime::input::gyro_y_sensitivity();
            ImGui::TextUnformatted("Vertical gyro sensitivity");
            ImGui::SetNextItemWidth(available_width);
            if (ControlSliderFloat("##gyro-y-sensitivity", &y_sensitivity,
                                   25.0F, 300.0F, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                dkr::runtime::input::set_gyro_y_sensitivity(y_sensitivity);
                SaveSettings();
            }
            float deadzone = dkr::runtime::input::gyro_deadzone();
            ImGui::TextUnformatted("Motion deadzone");
            ImGui::SetNextItemWidth(available_width);
            if (ControlSliderFloat("##gyro-deadzone", &deadzone,
                                   0.0F, 12.0F, "%.1f deg/s",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                dkr::runtime::input::set_gyro_deadzone(deadzone);
                SaveSettings();
            }
            bool inverted = dkr::runtime::input::gyro_inverted();
            if (ImGui::Checkbox("Invert horizontal gyro", &inverted)) {
                dkr::runtime::input::set_gyro_inverted(inverted);
                SaveSettings();
            }
            bool y_inverted = dkr::runtime::input::gyro_y_inverted();
            if (ImGui::Checkbox("Invert vertical gyro", &y_inverted)) {
                dkr::runtime::input::set_gyro_y_inverted(y_inverted);
                SaveSettings();
            }
            const bool available = dkr::runtime::platform::gyro_available();
            const float steering =
                dkr::runtime::input::gyro_steering_position();
            ImGui::ProgressBar((steering + 1.0F) * 0.5F,
                               {available_width, 18.0F},
                               "Horizontal steering");
            const float vertical =
                dkr::runtime::input::gyro_steering_y_position();
            ImGui::ProgressBar((vertical + 1.0F) * 0.5F,
                               {available_width, 18.0F},
                               "Vertical steering");
            ImGui::BeginDisabled(!live || !available);
            if (ImGui::Button("RECENTER STEERING", {available_width, 44.0F})) {
                dkr::runtime::input::recenter_gyro();
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!live || !available ||
                                 dkr::runtime::input::gyro_calibrating());
            if (ImGui::Button("CALIBRATE CONTROLLER", {available_width, 44.0F})) {
                dkr::runtime::input::begin_gyro_calibration();
            }
            ImGui::EndDisabled();
            if (dkr::runtime::input::gyro_calibrating()) {
                const float progress = dkr::runtime::input::gyro_calibration_progress();
                ImGui::ProgressBar(progress, {available_width, 18.0F},
                                   "Keep the controller still");
            } else if (!live) {
                ImGui::TextDisabled("Calibration is available from the in-game overlay.");
            } else if (!available) {
                ImGui::TextDisabled("No SDL gyro sensor was reported by Controller 1.");
            }
        }
    }

    constexpr const char* kCapturePopup = "CHOOSE A NEW CONTROL";
    if (g_capture_popup_pending) {
        ImGui::OpenPopup(kCapturePopup);
        g_capture_popup_pending = false;
    }
    ImGui::SetNextWindowSize({std::min(520.0F, ImGui::GetIO().DisplaySize.x - 32.0F), 230.0F},
                             ImGuiCond_Appearing);
    if (BeginPaddedModal(kCapturePopup,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (g_capture_action >= 0) {
            const auto action = static_cast<Action>(g_capture_action);
            PushHeadingFont();
            ImGui::TextWrapped("%s", dkr::runtime::input::action_label(action));
            PopHeadingFont();
            ImGui::TextWrapped(g_capture_device == CaptureDevice::Keyboard
                ? "Press a keyboard key. Escape cancels."
                : "Press a gamepad button or move an axis firmly. Escape cancels.");
        } else if (g_capture_action == kShortcutCaptureAction) {
            PushHeadingFont();
            ImGui::TextUnformatted("QUICK RESTART");
            PopHeadingFont();
            ImGui::TextWrapped(g_capture_device == CaptureDevice::Keyboard
                ? "Press one key, or hold the first and press a second. The chord is saved automatically. Escape cancels."
                : "Press one controller button, or hold the first and press a second. The chord is saved automatically. Escape cancels.");
            if (g_shortcut_capture_count == 1) {
                const auto pending = dkr::runtime::input::ShortcutBinding{
                    g_shortcut_capture_sources[0],
                    dkr::runtime::input::kUnbound};
                ImGui::Text("Captured: %s",
                            ShortcutBindingName(g_capture_device, pending).c_str());
            }
        }
        ImGui::Dummy({0.0F, 12.0F});
        const float popup_gap = ImGui::GetStyle().ItemSpacing.x;
        const float popup_button_width = std::max(
            (ImGui::GetContentRegionAvail().x - popup_gap) * 0.5F, 1.0F);
        if (ImGui::Button("UNBIND", {popup_button_width, 44.0F})) {
            if (g_capture_action >= 0) {
                const auto action = static_cast<Action>(g_capture_action);
                if (g_capture_device == CaptureDevice::Keyboard) {
                    dkr::runtime::input::set_keyboard_binding(
                        action, dkr::runtime::input::kUnbound);
                } else {
                    dkr::runtime::input::set_controller_binding(
                        action, dkr::runtime::input::kUnbound);
                }
            } else if (g_capture_action == kShortcutCaptureAction) {
                const dkr::runtime::input::ShortcutBinding unbound{};
                if (g_capture_device == CaptureDevice::Keyboard) {
                    dkr::runtime::input::set_quick_restart_keyboard_binding(unbound);
                } else {
                    dkr::runtime::input::set_quick_restart_controller_binding(unbound);
                }
            }
            SaveSettings();
            g_capture_finished = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", {popup_button_width, 44.0F})) {
            g_capture_finished = true;
        }
        if (g_capture_finished) {
            ImGui::CloseCurrentPopup();
            g_capture_action = -1;
            g_capture_device = CaptureDevice::None;
            g_capture_finished = false;
        }
        ImGui::EndPopup();
    }
    ImGui::Dummy({0.0F, 44.0F});
}

bool HandleInputCaptureEvent(SDL_Event* event) {
    if (event == nullptr || g_capture_action == -1 || g_capture_finished) {
        return false;
    }
    if (g_capture_action == kShortcutCaptureAction) {
        if (event->type == SDL_KEYDOWN && event->key.repeat == 0 &&
            event->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_capture_finished = true;
            return true;
        }
        int source = dkr::runtime::input::kUnbound;
        if (g_capture_device == CaptureDevice::Keyboard &&
            event->type == SDL_KEYDOWN && event->key.repeat == 0) {
            source = static_cast<int>(event->key.keysym.scancode);
        } else if (g_capture_device == CaptureDevice::Controller &&
                   event->type == SDL_CONTROLLERBUTTONDOWN) {
            source = dkr::runtime::input::encode_controller_button(
                event->cbutton.button);
        }
        if (source != dkr::runtime::input::kUnbound &&
            (g_shortcut_capture_count == 0 ||
             source != g_shortcut_capture_sources[0])) {
            g_shortcut_capture_sources[g_shortcut_capture_count++] = source;
            if (g_shortcut_capture_count >= 2) {
                CommitShortcutCapture();
            } else {
                g_shortcut_capture_deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(800);
            }
            return true;
        }
        return false;
    }
    using dkr::runtime::input::Action;
    const auto action = static_cast<Action>(g_capture_action);
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0) {
        if (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_capture_finished = true;
            return true;
        }
        if (g_capture_device == CaptureDevice::Keyboard) {
            dkr::runtime::input::set_keyboard_binding(
                action, static_cast<int>(event->key.keysym.scancode));
            SaveSettings();
            g_capture_finished = true;
            return true;
        }
    }
    if (g_capture_device == CaptureDevice::Controller &&
        event->type == SDL_CONTROLLERBUTTONDOWN) {
        dkr::runtime::input::set_controller_binding(
            action, dkr::runtime::input::encode_controller_button(event->cbutton.button));
        SaveSettings();
        g_capture_finished = true;
        return true;
    }
    if (g_capture_device == CaptureDevice::Controller &&
        event->type == SDL_CONTROLLERAXISMOTION && std::abs(event->caxis.value) >= 20000) {
        dkr::runtime::input::set_controller_binding(
            action, dkr::runtime::input::encode_controller_axis(
                        event->caxis.axis, event->caxis.value > 0));
        SaveSettings();
        g_capture_finished = true;
        return true;
    }
    return false;
}

void DrawOverlayContent(float content_width) {
    if (g_overlay_page == kPagePlay) {
        DrawPageHeading("PLAY");
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("Taj has paused the race. The island is waiting whenever you are ready.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0F, 18.0F});
        DrawStartingLights(true);
        ImGui::Dummy({0.0F, 12.0F});
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kWarm);
        if (ImGui::Button("RETURN TO THE RACE", {content_width, 64.0F})) {
            g_overlay_visible.store(false, std::memory_order_release);
        }
        ImGui::PopStyleColor(2);
    } else if (g_overlay_page == kPageGraphics) {
        DrawPageHeading("GRAPHICS");
        ImGui::Separator();
        DrawGraphicsSettings(true);
    } else if (g_overlay_page == kPageSound) {
        DrawPageHeading("SOUND");
        ImGui::TextDisabled("Mix DKR-R in real time without changing game timing.");
        ImGui::Dummy({0.0F, 20.0F});
        DrawAudioSettings(content_width);
    } else if (g_overlay_page == kPageControls) {
        DrawPageHeading("CONTROLS");
        ImGui::Separator();
        DrawControlsReference(true);
        ImGui::Spacing();
        ImGui::SeparatorText("Controller and Pak");
        bool rumble = dkr::runtime::platform::rumble_enabled();
        if (ImGui::Checkbox("Controller rumble", &rumble)) {
            dkr::runtime::platform::set_rumble_enabled(rumble);
            SaveSettings();
        }
        if (rumble && dkr::runtime::enhancements::modern_options_visible(
                          dkr::runtime::enhancements::presentation_profile())) {
            float rumble_percent = dkr::runtime::platform::rumble_strength() * 100.0F;
            ImGui::TextUnformatted("Rumble strength");
            ImGui::SetNextItemWidth(std::min(content_width, 720.0F));
            if (ControlSliderFloat("##rumble-strength", &rumble_percent,
                                   0.0F, 100.0F, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                dkr::runtime::platform::set_rumble_strength(rumble_percent / 100.0F);
                SaveSettings();
            }
        }
        bool memory_pak = dkr::runtime::pak::enabled();
        if (ImGui::Checkbox("Virtual Memory Pak", &memory_pak)) {
            dkr::runtime::pak::set_enabled(memory_pak);
            SaveSettings();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("T.T. keeps Controller Pak data safely in your DKR-R settings folder, with a recovery backup after every successful write.");
        ImGui::PopStyleColor();
    } else if (g_overlay_page == kPageSaveManager) {
        DrawPageHeading("SAVE MANAGER");
        ImGui::TextDisabled("Save transfers are locked while the game owns the EEPROM.");
        ImGui::Dummy({0.0F, 12.0F});
        DrawSaveManager(true);
    } else if (g_overlay_page == kPageOnlineMp) {
        DrawComingSoonPage(
            "ONLINE MP", "online-mp-coming-soon",
            "Online multiplayer will let racers meet beyond the shores of DKR-R.",
            "Taj is still tuning the network karts and testing every shortcut.",
            content_width);
    } else if (g_overlay_page == kPageModsHacks) {
        DrawComingSoonPage(
            "MODS / HACKS", "mods-hacks-coming-soon",
            "A dedicated garage for community mods and game hacks is planned for DKR-R.",
            "T.T. is still checking every part before the mod garage opens.",
            content_width);
    } else if (g_overlay_page == kPageAbout) {
        DrawAboutDkrR(content_width);
    }
    ImGui::Dummy({0.0F, 44.0F});
}

} // namespace

void dkr::runtime::ui::configure(const std::filesystem::path& config_directory) {
    g_config_directory = config_directory;
    dkr::runtime::magic_codes::configure(config_directory);
    dkr::runtime::texture_packs::configure(config_directory);
    RefreshCrtFilters();
    LoadSettings();
    if (!g_crt_filters.empty()) {
        g_crt_filter_index = std::clamp(
            g_crt_filter_index, 0, static_cast<int>(g_crt_filters.size()) - 1);
    }
}

void dkr::runtime::ui::persist_settings() {
    SaveSettings();
}

void dkr::runtime::ui::persist_graphics_api_fallback() {
    GraphicsConfig config = ultramodern::renderer::get_graphics_config();
    config.api_option = GraphicsApi::Auto;
    g_modern_graphics_api = GraphicsApi::Auto;
    ultramodern::renderer::set_graphics_config(config);
    SaveSettings();
    std::fprintf(stderr,
                 "[boot][settings] unavailable graphics API recovered to Automatic\n");
}

dkr::runtime::ui::StartupResult dkr::runtime::ui::run_startup_screen(SDL_Window* window) {
    StartupResult result{};
    if (window == nullptr) {
        return result;
    }

    SDL_SetWindowTitle(window, "DKR-R - Diddy Kong Racing Recompiled");
    // SDL's accelerated Linux renderers can replace the native window state
    // used by SDL_Vulkan_CreateSurface. That leaves the launcher visible but
    // makes the window disappear as soon as RT64 takes over under Gamescope.
    // The launcher is inexpensive 2D UI, so keep it on the software backend
    // and reserve the Vulkan-capable window for the game renderer.
#if defined(__linux__)
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
#else
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
#endif
    if (renderer == nullptr) {
        std::fprintf(stderr, "[boot][launcher] SDL renderer failed: %s\n", SDL_GetError());
        return result;
    }
    SDL_RendererInfo renderer_info{};
    if (SDL_GetRendererInfo(renderer, &renderer_info) == 0) {
        std::fprintf(stderr,
                     "[boot][launcher] SDL renderer=%s accelerated=%s "
                     "window-flags=0x%08X\n",
                     renderer_info.name != nullptr ? renderer_info.name : "unknown",
                     (renderer_info.flags & SDL_RENDERER_ACCELERATED) != 0 ? "yes" : "no",
                     static_cast<unsigned>(SDL_GetWindowFlags(window)));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    LoadLauncherFonts();
    ApplyStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    std::filesystem::path selected_rom;
    std::string rom_status = "Choose your legally obtained Diddy Kong Racing Game Pak.";
    bool rom_ready = false;
    if (const auto remembered = LoadLastRom(); remembered.has_value()) {
        std::string error;
        if (dkr::runtime::SelectRom(*remembered, error)) {
            selected_rom = *remembered;
            rom_ready = true;
            rom_status = "Game Pak ready. The adventure can begin!";
        } else {
            rom_status = "T.T. could not find the previous Game Pak. Choose it again.";
        }
    }

    int page = 0;
    bool focus_selected_tab = true;
    bool running = true;
    bool launch_requested = false;
    while (running) {
        bool request_quit_popup = false;
        bool request_restart_popup = false;
        constexpr int launcher_page_count = kMenuPageCount;
        if (page >= launcher_page_count) {
            page = 0;
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            dkr::runtime::platform::update_fullscreen_cursor(&event);
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (HandleInputCaptureEvent(&event)) {
                continue;
            }
            if (dkr::runtime::platform::handle_window_shortcut(&event, false)) {
                continue;
            }
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                running = false;
            }
            if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                event.key.keysym.scancode == SDL_SCANCODE_ESCAPE && g_rom_browser.open) {
                g_rom_browser.close_requested = true;
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B && g_rom_browser.open) {
                    RomBrowserBack();
                } else if (!g_rom_browser.open &&
                           event.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                    page = (page + launcher_page_count - 1) % launcher_page_count;
                    focus_selected_tab = true;
                } else if (!g_rom_browser.open &&
                           event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                    page = (page + 1) % launcher_page_count;
                    focus_selected_tab = true;
                } else if (!g_rom_browser.open && page == 0 && rom_ready &&
                           event.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                    launch_requested = true;
                }
            }
        }
        dkr::runtime::platform::update_fullscreen_cursor();

        if (launch_requested) {
            result.start_game = true;
            result.rom_path = selected_rom;
            running = false;
            continue;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        dkr::runtime::platform::update_ui_gamepad_navigation();
        ImGui::NewFrame();
        BeginMainWindow("DKR-R Startup");
        DrawRaceBackdrop(false);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float outer_margin = std::clamp(available.x * 0.022F, 16.0F, 34.0F);
        const float content_height = available.y - outer_margin * 2.0F;
        const float panel_gap = std::clamp(available.x * 0.018F, 14.0F, 28.0F);
        const float minimum_sidebar = available.x < 980.0F ? 190.0F : 230.0F;
        const float sidebar_width = std::clamp(
            available.x * 0.235F, minimum_sidebar,
            std::min(340.0F, available.x * 0.34F));
        const float right_width = std::max(
            available.x - outer_margin * 2.0F - sidebar_width - panel_gap,
            320.0F);
        const float panel_padding = std::clamp(right_width * 0.045F, 20.0F, 44.0F);
        const float right_inner_width = std::max(right_width - panel_padding * 2.0F, 1.0F);
        ImGui::SetCursorPos({outer_margin, outer_margin});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.025F, 0.105F, 0.15F, 0.96F});
        ImGui::PushStyleColor(ImGuiCol_Border, {1.0F, 0.67F, 0.08F, 0.92F});
        ImGui::BeginChild("launcher-nav", {sidebar_width, content_height}, true,
                          ImGuiWindowFlags_NavFlattened);
        const float nav_padding = sidebar_width < 220.0F ? 16.0F : 24.0F;
        const float nav_inner_width = sidebar_width - nav_padding * 2.0F;
        ImGui::SetCursorPos({nav_padding, nav_padding});
        ImGui::PushTextWrapPos(sidebar_width - nav_padding);
        ImGui::BeginGroup();
        BrandBlock(nav_inner_width, content_height < 760.0F ? 170.0F : 218.0F);
        ImGui::Dummy({0.0F, 18.0F});
        LauncherSidebarButton("PLAY", 0, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("GRAPHICS", 1, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("SOUND", 2, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("CONTROLS", 3, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("SAVE MANAGER", 4, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("ONLINE MP", 5, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("MODS / HACKS", 6, page, focus_selected_tab,
                              nav_inner_width);
        LauncherSidebarButton("ABOUT DKR-R", 7, page, focus_selected_tab,
                              nav_inner_width);
        focus_selected_tab = false;
        ImGui::Dummy({0.0F, 18.0F});
        DrawSidebarNote(nav_inner_width);
        ImGui::Dummy({0.0F, 16.0F});
        ImGui::PushStyleColor(ImGuiCol_Button, {0.92F, 0.43F, 0.06F, 1.0F});
        if (ImGui::Button("RESTART DKR-R", {nav_inner_width, 44.0F})) {
            request_restart_popup = true;
        }
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0F, 8.0F});
        ImGui::PushStyleColor(ImGuiCol_Button, kRaceRed);
        if (ImGui::Button("EXIT DKR-R", {nav_inner_width, 44.0F})) {
            request_quit_popup = true;
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0F, panel_gap);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.035F, 0.085F, 0.12F, 0.98F});
        ImGui::PushStyleColor(ImGuiCol_Border, {0.12F, 0.62F, 0.58F, 0.88F});
        ImGui::BeginChild("launcher-content", {right_width, content_height}, true,
                          ImGuiWindowFlags_NavFlattened);
        ImGui::SetCursorPos({panel_padding, panel_padding});
        ImGui::PushItemWidth(right_inner_width);
        ImGui::PushTextWrapPos(panel_padding + right_inner_width);
        ImGui::BeginGroup();
        if (page == 0) {
            DrawPageHeading("PLAY");
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped(rom_ready
                ? "The starting lights are green. DKR-R is ready."
                : "Choose your legally obtained Game Pak and join the race to stop Wizpig.");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.0F, 16.0F});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kCream);
            ImGui::PushStyleColor(ImGuiCol_Border, rom_ready ? kAccent : kRaceRed);
            BeginPaddedChild("race-pass", {right_inner_width, 316.0F}, true, 0,
                             {24.0F, 20.0F});
            ImGui::PushTextWrapPos(right_inner_width - 24.0F);
            ImGui::BeginGroup();
            DrawStartingLights(rom_ready);
            ImGui::SameLine(0.0F, 18.0F);
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, rom_ready
                ? ImVec4{0.01F, 0.48F, 0.28F, 1.0F} : kRaceRed);
            ImGui::TextUnformatted(rom_ready ? "ENTRY CLEARED" : "GAME DATA NEEDED");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, {0.035F, 0.105F, 0.14F, 1.0F});
            ImGui::TextWrapped("%s", rom_status.c_str());
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            if (!selected_rom.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, {0.18F, 0.31F, 0.34F, 1.0F});
                ImGui::TextWrapped("%s", PathUtf8(selected_rom).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy({0.0F, 6.0F});
            if (ImGui::Button(rom_ready ? "CHOOSE ANOTHER GAME PAK" : "CHOOSE YOUR GAME PAK",
                              {std::min(260.0F, right_inner_width - 48.0F), 46.0F})) {
                OpenRomBrowser(selected_rom);
            }
            ImGui::EndGroup();
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::Dummy({0.0F, 20.0F});
            ImGui::BeginDisabled(!rom_ready);
            ImGui::PushStyleColor(ImGuiCol_Button, kRaceRed);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kWarm);
            if (ImGui::Button("START Diddy Kong Racing - Recompiled",
                              {right_inner_width, 68.0F})) {
                result.start_game = true;
                result.rom_path = selected_rom;
                running = false;
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
        } else if (page == 1) {
            DrawPageHeading("GRAPHICS");
            ImGui::TextDisabled("Tune the view and presentation for your machine.");
            ImGui::Dummy({0.0F, 18.0F});
            DrawGraphicsSettings(false);
        } else if (page == 2) {
            DrawPageHeading("SOUND");
            ImGui::TextDisabled("Balance music, vehicles, effects and island ambience.");
            ImGui::Dummy({0.0F, 18.0F});
            DrawAudioSettings(right_inner_width);
        } else if (page == 3) {
            DrawPageHeading("CONTROLS");
            ImGui::TextDisabled("Keyboard or gamepad - pick your machine and hit the track.");
            ImGui::Dummy({0.0F, 12.0F});
            DrawControlsReference(false);
        } else if (page == 4) {
            DrawPageHeading("SAVE MANAGER");
            ImGui::TextDisabled("Back up, import, export or build Adventure progress before racing.");
            ImGui::Dummy({0.0F, 12.0F});
            DrawSaveManager(false);
        } else if (page == kPageOnlineMp) {
            DrawComingSoonPage(
                "ONLINE MP", "launcher-online-mp-coming-soon",
                "Online multiplayer will let racers meet beyond the shores of DKR-R.",
                "Taj is still tuning the network karts and testing every shortcut.",
                right_inner_width);
        } else if (page == kPageModsHacks) {
            DrawComingSoonPage(
                "MODS / HACKS", "launcher-mods-hacks-coming-soon",
                "A dedicated garage for community mods and game hacks is planned for DKR-R.",
                "T.T. is still checking every part before the mod garage opens.",
                right_inner_width);
        } else {
            DrawAboutDkrR(right_inner_width);
        }
        ImGui::Dummy({0.0F, 54.0F});
        ImGui::EndGroup();
        ImGui::PopTextWrapPos();
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        DrawRomBrowser(selected_rom, rom_status, rom_ready);
        if (request_restart_popup) {
            ImGui::OpenPopup("Restart DKR-R?");
        }
        if (request_quit_popup) {
            ImGui::OpenPopup("Quit DKR-R?");
        }
        if (BeginPaddedModal("Restart DKR-R?", ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Restart DKR-R and reload the launcher?");
            ImGui::TextDisabled("Your selected Game Pak and settings will be preserved.");
            if (ImGui::Button("CANCEL", {120.0F, 40.0F})) ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.92F, 0.43F, 0.06F, 1.0F});
            if (ImGui::Button("RESTART DKR-R", {170.0F, 40.0F})) {
                SaveSettings();
                result.lifecycle_request = LifecycleRequest::Restart;
                running = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        if (BeginPaddedModal("Quit DKR-R?", ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Exit DKR-R and return to the desktop?");
            if (ImGui::Button("CANCEL", {120.0F, 40.0F})) ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.45F, 0.09F, 0.10F, 1.0F});
            if (ImGui::Button("EXIT DKR-R", {140.0F, 40.0F})) {
                SaveSettings();
                result.lifecycle_request = LifecycleRequest::Exit;
                running = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        ImGui::End();

        ImGui::Render();
        // The window is created with SDL_WINDOW_ALLOW_HIGHDPI, so on a Retina
        // display the renderer's output is in pixels while ImGui lays out and
        // emits vertices in points. Without this the UI draws into the
        // top-left quadrant at half scale, and because
        // ImGui_ImplSDLRenderer2_RenderDrawData scales its clip rects by
        // FramebufferScale, the mismatched clip rects also shear off the left
        // edge of the sidebar text. Scaling the renderer puts vertices and
        // clip rects back in the same space.
        const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
        SDL_RenderSetScale(renderer, framebuffer_scale.x, framebuffer_scale.y);
        SDL_SetRenderDrawColor(renderer, 6, 9, 14, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    std::fprintf(stderr,
                 "[boot][launcher] handoff complete window-flags=0x%08X\n",
                 static_cast<unsigned>(SDL_GetWindowFlags(window)));
    return result;
}

void dkr::runtime::ui::attach(RT64::Application& application) {
    if (application.presentQueue == nullptr || application.device == nullptr ||
        application.swapChain == nullptr) {
        return;
    }
    std::scoped_lock guard(g_inspector_guard);
    std::scoped_lock present_lock(application.presentQueue->inspectorMutex);
    application.presentQueue->inspector = std::make_unique<RT64::Inspector>(
        application.device.get(), application.swapChain.get(),
        application.chosenGraphicsAPI,
        static_cast<SDL_Window*>(dkr::runtime::platform::sdl_window()));
    application.presentQueue->inspector->setIniPath(g_config_directory / "dkr-port-ui.ini");
    g_inspector = application.presentQueue->inspector.get();
    ImGui::GetIO().ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    LoadLauncherFonts();
    std::fprintf(stderr, "[boot][ui] in-game overlay attached (F1/Escape/Back)\n");
}

void dkr::runtime::ui::detach(RT64::Application& application) {
    std::scoped_lock guard(g_inspector_guard);
    g_inspector = nullptr;
    if (application.presentQueue != nullptr) {
        std::scoped_lock present_lock(application.presentQueue->inspectorMutex);
        dkr::runtime::crt::release();
        application.presentQueue->inspector.reset();
    }
}

void dkr::runtime::ui::draw(RT64::Application& application) {
    if (application.presentQueue == nullptr || application.framebufferGraphicsWorker == nullptr) {
        return;
    }
    const bool show_overlay =
        g_overlay_visible.load(std::memory_order_acquire);
    const bool show_fps = g_fps_overlay_enabled &&
        dkr::runtime::enhancements::modern_presentation_enabled();
    const bool show_crt = g_crt_enabled && g_crt_strength > 0.0F &&
        dkr::runtime::enhancements::modern_presentation_enabled() &&
        g_crt_filter_index >= 0 &&
        g_crt_filter_index < static_cast<int>(g_crt_filters.size());
    if (!show_overlay && !show_fps && !show_crt) {
        if (application.presentQueue->inspector != nullptr) {
            detach(application);
        }
        return;
    }
    if (application.presentQueue->inspector == nullptr) {
        attach(application);
        if (show_overlay) {
            g_overlay_focus_requested.store(true, std::memory_order_release);
        }
    }
    if (application.presentQueue->inspector == nullptr) {
        return;
    }
    if (show_overlay) {
        dkr::runtime::platform::update_ui_gamepad_navigation();
    }
    RT64::Inspector* inspector = application.presentQueue->inspector.get();
    inspector->newFrame(application.framebufferGraphicsWorker.get());
    ApplyStyle();
    ImGui::GetIO().ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (show_crt) {
        const CrtFilterEntry& filter =
            g_crt_filters[static_cast<std::size_t>(g_crt_filter_index)];
        dkr::runtime::crt::draw(
            application, filter.path,
            g_crt_scale_mode == 1 ? dkr::runtime::crt::ScaleMode::Tile
                                  : dkr::runtime::crt::ScaleMode::Stretch,
            g_crt_strength, g_crt_status);
    }
    if (show_overlay) {
    BeginMainWindow("DKR-R Overlay", ImGuiWindowFlags_NoBackground);
        bool request_quit_popup = false;
        bool request_restart_popup = false;
        const bool focus_selected_page =
            g_overlay_focus_requested.exchange(false, std::memory_order_acq_rel);
        const float overlay_margin = std::clamp(ImGui::GetWindowWidth() * 0.025F, 12.0F, 38.0F);
        const float overlay_gap = std::clamp(ImGui::GetWindowWidth() * 0.018F, 12.0F, 28.0F);
        const float minimum_sidebar = ImGui::GetWindowWidth() < 1000.0F ? 190.0F : 220.0F;
        const float maximum_sidebar = std::max(ImGui::GetWindowWidth() * 0.40F, minimum_sidebar);
        const float sidebar_width = std::clamp(ImGui::GetWindowWidth() * 0.25F,
                                               minimum_sidebar, std::min(370.0F, maximum_sidebar));
        const float overlay_height = ImGui::GetWindowHeight() - overlay_margin * 2.0F;
        const float content_x = overlay_margin + sidebar_width + overlay_gap;
        const float content_panel_width = ImGui::GetWindowWidth() - content_x - overlay_margin;
        ImGui::SetCursorPos({overlay_margin, overlay_margin});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.025F, 0.09F, 0.13F, 0.84F});
        ImGui::PushStyleColor(ImGuiCol_Border, kWarm);
        ImGui::BeginChild("overlay-nav", {sidebar_width, overlay_height}, true,
                          ImGuiWindowFlags_NavFlattened);
        const float nav_padding = sidebar_width < 220.0F ? 16.0F : 24.0F;
        ImGui::SetCursorPos({nav_padding, nav_padding});
        const float nav_inner_width = sidebar_width - nav_padding * 2.0F;
        ImGui::PushTextWrapPos(sidebar_width - nav_padding);
        ImGui::BeginGroup();
        BrandBlock(nav_inner_width, overlay_height < 800.0F ? 150.0F : 190.0F);
        ImGui::Dummy({0.0F, 18.0F});
        if (focus_selected_page && g_overlay_page == 0) ImGui::SetKeyboardFocusHere();
        SidebarButton("PLAY", 0, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 1) ImGui::SetKeyboardFocusHere();
        SidebarButton("GRAPHICS", 1, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 2) ImGui::SetKeyboardFocusHere();
        SidebarButton("SOUND", 2, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 3) ImGui::SetKeyboardFocusHere();
        SidebarButton("CONTROLS", 3, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 4) ImGui::SetKeyboardFocusHere();
        SidebarButton("SAVE MANAGER", 4, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 5) ImGui::SetKeyboardFocusHere();
        SidebarButton("ONLINE MP", 5, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 6) ImGui::SetKeyboardFocusHere();
        SidebarButton("MODS / HACKS", 6, nav_inner_width);
        if (focus_selected_page && g_overlay_page == 7) ImGui::SetKeyboardFocusHere();
        SidebarButton("ABOUT DKR-R", 7, nav_inner_width);
        ImGui::Dummy({0.0F, 16.0F});
        DrawSidebarNote(nav_inner_width);
        ImGui::Dummy({0.0F, 16.0F});
        ImGui::PushStyleColor(ImGuiCol_Button, {0.92F, 0.43F, 0.06F, 1.0F});
        if (ImGui::Button("RESTART DKR-R", {nav_inner_width, 44.0F})) {
            request_restart_popup = true;
        }
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0F, 8.0F});
        ImGui::PushStyleColor(ImGuiCol_Button, kRaceRed);
        const bool leave_island_pressed =
            ImGui::Button("EXIT DKR-R", {nav_inner_width, 44.0F});
        if (leave_island_pressed) {
            request_quit_popup = true;
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::SetCursorPos({content_x, overlay_margin});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.035F, 0.085F, 0.12F, 0.84F});
        ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
        ImGui::BeginChild("overlay-content", {content_panel_width, overlay_height}, true,
                          ImGuiWindowFlags_NavFlattened);
        const float content_padding = std::clamp(content_panel_width * 0.045F, 20.0F, 44.0F);
        const float content_inner_width = content_panel_width - content_padding * 2.0F;
        ImGui::SetCursorPos({content_padding, content_padding});
        ImGui::PushItemWidth(content_inner_width);
        ImGui::PushTextWrapPos(content_padding + content_inner_width);
        ImGui::BeginGroup();
        DrawOverlayContent(content_inner_width);
        ImGui::EndGroup();
        ImGui::PopTextWrapPos();
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        if (request_quit_popup) {
            // Open the modal in the same parent ID scope where it is rendered.
            // Opening it inside overlay-nav creates a different ImGui popup ID,
            // which made the Exit to Desktop button appear to do nothing.
            ImGui::OpenPopup("Quit DKR-R?");
        }
        if (request_restart_popup) {
            ImGui::OpenPopup("Restart DKR-R?");
        }
        if (BeginPaddedModal("Restart DKR-R?", ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Restart DKR-R and return to the launcher?");
            ImGui::TextDisabled("Progress since the last in-game save point may be lost.");
            if (ImGui::Button("CANCEL", {120.0F, 40.0F})) ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.92F, 0.43F, 0.06F, 1.0F});
            if (ImGui::Button("RESTART DKR-R", {170.0F, 40.0F})) {
                SaveSettings();
                g_lifecycle_request.store(
                    dkr::runtime::ui::LifecycleRequest::Restart,
                    std::memory_order_release);
                g_overlay_visible.store(false, std::memory_order_release);
                ultramodern::quit();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        if (BeginPaddedModal("Quit DKR-R?", ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Exit DKR-R and return to the desktop?");
            ImGui::TextDisabled("Progress since the last in-game save point may be lost.");
            if (ImGui::Button("CANCEL", {120.0F, 40.0F})) ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.45F, 0.09F, 0.10F, 1.0F});
            const bool confirm_leave_pressed = ImGui::Button("EXIT DKR-R", {140.0F, 40.0F});
            if (confirm_leave_pressed) {
                SaveSettings();
                g_lifecycle_request.store(
                    dkr::runtime::ui::LifecycleRequest::Exit,
                    std::memory_order_release);
                g_overlay_visible.store(false, std::memory_order_release);
                ultramodern::quit();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
    ImGui::End();
    }
    DrawFpsOverlay(application);
    inspector->endFrame();
}

bool dkr::runtime::ui::handle_runtime_event(SDL_Event* event) {
    if (event == nullptr) {
        return false;
    }
    if (HandleInputCaptureEvent(event)) {
        return true;
    }
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0 &&
        event->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        toggle_overlay();
        return true;
    }
    if (g_overlay_visible.load(std::memory_order_acquire) &&
        event->type == SDL_CONTROLLERBUTTONDOWN) {
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
            const int page = g_overlay_page.load(std::memory_order_relaxed);
            g_overlay_page.store((page + kMenuPageCount - 1) % kMenuPageCount,
                                 std::memory_order_relaxed);
            g_overlay_focus_requested.store(true, std::memory_order_release);
            return true;
        }
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
            const int page = g_overlay_page.load(std::memory_order_relaxed);
            g_overlay_page.store((page + 1) % kMenuPageCount,
                                 std::memory_order_relaxed);
            g_overlay_focus_requested.store(true, std::memory_order_release);
            return true;
        }
    }
    std::scoped_lock guard(g_inspector_guard);
    if (g_inspector == nullptr) {
        return false;
    }
    std::scoped_lock frame_lock(g_inspector->frameMutex);
    // RT64's ImGui backend owns normal pointer, keyboard and gamepad event
    // delivery. Do not turn those events into page-focus requests: ImGui must
    // be allowed to retain the item selected by the previous event so the
    // player can move from the sidebar into sliders, combos and buttons.
    return g_inspector->handleSdlEvent(event);
}

bool dkr::runtime::ui::input_capture_active() {
    return g_capture_action != -1 && !g_capture_finished;
}

void dkr::runtime::ui::toggle_overlay() {
    const bool next = !g_overlay_visible.load(std::memory_order_acquire);
    g_overlay_page = 0;
    g_overlay_visible.store(next, std::memory_order_release);
    g_overlay_focus_requested.store(next, std::memory_order_release);
}

bool dkr::runtime::ui::overlay_visible() {
    return g_overlay_visible.load(std::memory_order_acquire);
}

dkr::runtime::ui::LifecycleRequest dkr::runtime::ui::lifecycle_request() {
    return g_lifecycle_request.load(std::memory_order_acquire);
}

void dkr::runtime::ui::reset_lifecycle_request() {
    g_lifecycle_request.store(LifecycleRequest::None, std::memory_order_release);
}
