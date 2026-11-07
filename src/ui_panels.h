#pragma once
#include "ui_widgets.h"

namespace ui {
inline ImVec4 rgb(int r, int g, int b) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

struct PanelStyle {
    float s = ui::textScale();
    ImVec4 background, surface, raised, border, text, muted, blue, green, red, gold;

    explicit PanelStyle(bool light) {
        background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        surface = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
        raised = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        border = light ? rgb(174, 197, 221) : rgb(45, 61, 82);
        text = light ? rgb(26, 45, 68) : rgb(244, 247, 252);
        muted = light ? rgb(74, 98, 124) : rgb(180, 195, 215);
        blue = light ? rgb(0, 111, 188) : rgb(71, 208, 255);
        green = light ? rgb(0, 127, 87) : rgb(39, 234, 161);
        red = light ? rgb(184, 52, 64) : rgb(255, 133, 143);
        gold = light ? rgb(145, 106, 10) : rgb(233, 194, 48);
    }

    ImU32 color(ImVec4 value) const { return ImGui::GetColorU32(value); }

    void box(ImVec2 p, ImVec2 size, ImVec4 top, ImVec4 bottom, ImVec4 edge, float radius = 9) const {
        auto* draw = ImGui::GetWindowDrawList();
        int first = draw->VtxBuffer.Size;
        draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), color(top), radius * s);
        for (int i = first; i < draw->VtxBuffer.Size; ++i) {
            auto& vertex = draw->VtxBuffer[i];
            float t = std::clamp((vertex.pos.y - p.y) / std::max(1.0f, size.y), 0.0f, 1.0f);
            ImVec4 shade(top.x + (bottom.x - top.x) * t, top.y + (bottom.y - top.y) * t,
                top.z + (bottom.z - top.z) * t, top.w + (bottom.w - top.w) * t);
            ImU32 alpha = vertex.col & IM_COL32_A_MASK;
            vertex.col = (color(shade) & ~IM_COL32_A_MASK) | alpha;
        }
        draw->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), color(edge), radius * s, 0, s);
    }

    void label(ImVec2 p, const std::string& value, float width, ImVec4 ink,
        float size = 14, bool bold = false) const {
        if (width <= 0) return;
        ImFont* font = bold && ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
        if (size >= 24 && ui::connectionHeadingFont) font = ui::connectionHeadingFont;
        float fontSize = size * s;
        std::string visible = value;
        if (font->CalcTextSizeA(fontSize, FLT_MAX, 0, value.c_str()).x > width) {
            const char* end = value.c_str();
            float dots = font->CalcTextSizeA(fontSize, FLT_MAX, 0, "...").x;
            font->CalcTextSizeA(fontSize, std::max(1.0f, width - dots), 0, value.c_str(), nullptr, &end);
            visible.assign(value.c_str(), end);
            visible += "...";
        }
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(p, ImVec2(p.x + width, p.y + fontSize + 3 * s), true);
        draw->AddText(font, fontSize, p, color(ink), visible.c_str());
        draw->PopClipRect();
    }

    // Icon tile tinted with an accent color, with a soft glow behind it.
    void tile(ui::Icon glyph, ImVec2 p, float size, ImVec4 tint) const {
        const float edge = size * s;
        const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
        auto blend = [&](float amount) {
            return ImVec4(base.x + (tint.x - base.x) * amount, base.y + (tint.y - base.y) * amount,
                base.z + (tint.z - base.z) * amount, 1.0f);
        };
        ImVec4 glow = tint; glow.w = 0.10f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 2 * s, p.y - 1 * s), ImVec2(p.x + edge + 2 * s, p.y + edge + 4 * s),
            color(glow), 12 * s);
        ImVec4 rim = tint; rim.w = 0.45f;
        box(p, ImVec2(edge, edge), blend(0.30f), blend(0.12f), rim, 10);
        const ImVec4 ink = ImVec4(std::min(1.0f, tint.x * 0.55f + 0.45f), std::min(1.0f, tint.y * 0.55f + 0.45f),
            std::min(1.0f, tint.z * 0.55f + 0.45f), 1.0f);
        ui::icon(glyph, ImVec2(p.x + size * .22f * s, p.y + size * .22f * s), size * .56f * s, color(ink));
    }

    void tile(ui::Icon glyph, ImVec2 p, float size = 40) const { tile(glyph, p, size, blue); }

    void heading(ImVec2 p, float width, ui::Icon glyph, const char* title, const char* subtitle) const {
        heading(p, width, glyph, title, subtitle, blue);
    }

    void heading(ImVec2 p, float width, ui::Icon glyph, const char* title, const char* subtitle, ImVec4 tint) const {
        tile(glyph, p, 40, tint);
        label(ImVec2(p.x + 58 * s, p.y + 1 * s), title, width - 58 * s, text, 17, true);
        label(ImVec2(p.x + 58 * s, p.y + 26 * s), subtitle, width - 58 * s, muted, 13);
    }

    // Section card: gradient body plus a thin tinted highlight along the top edge.
    void card(ImVec2 p, ImVec2 size, ImVec4 tint, float radius = 12) const {
        box(p, size, surface, background, border, radius);
        auto* draw = ImGui::GetWindowDrawList();
        const float inset = radius * s;
        const float mid = p.x + size.x * 0.5f;
        ImVec4 bright = tint; bright.w = 0.75f;
        ImVec4 clear = tint; clear.w = 0.0f;
        draw->AddRectFilledMultiColor(ImVec2(p.x + inset, p.y), ImVec2(mid, p.y + 2 * s),
            color(clear), color(bright), color(bright), color(clear));
        draw->AddRectFilledMultiColor(ImVec2(mid, p.y), ImVec2(p.x + size.x - inset, p.y + 2 * s),
            color(bright), color(clear), color(clear), color(bright));
    }

    ImVec2 pillSize(const char* value, bool withIcon = true) const {
        const float textWidth = ImGui::CalcTextSize(value).x;
        return ImVec2(textWidth + (withIcon ? 15 * s + 6 * s : 0) + 22 * s, 26 * s);
    }

    // Rounded badge with optional icon; icon and text are centered as one group.
    void pill(ImVec2 p, ImVec2 size, ui::Icon glyph, const char* value, ImVec4 ink) const {
        auto* draw = ImGui::GetWindowDrawList();
        ImVec4 fill = ink; fill.w = 0.13f;
        ImVec4 edge = ink; edge.w = 0.30f;
        draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), color(fill), size.y * 0.5f);
        draw->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), color(edge), size.y * 0.5f, 0, 1.0f);
        const ImVec2 textSize = ImGui::CalcTextSize(value);
        const float iconSize = glyph == ui::Icon::None ? 0 : 15 * s;
        const float gap = iconSize > 0 ? 6 * s : 0;
        float x = p.x + (size.x - textSize.x - iconSize - gap) * 0.5f;
        if (iconSize > 0) ui::icon(glyph, ImVec2(x, p.y + (size.y - iconSize) * 0.5f), iconSize, color(ink));
        draw->AddText(ImVec2(x + iconSize + gap, p.y + (size.y - textSize.y) * 0.5f), color(ink), value);
    }

    bool button(const char* id, const char* title, ui::Icon glyph, ImVec2 p, ImVec2 size,
        ImVec4 ink, int treatment = 0) const {
        ImGui::SetCursorScreenPos(p);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        bool pressed = ImGui::Button(id, size);
        ImGui::PopStyleColor(3);
        bool hovered = ImGui::IsItemHovered();
        ImVec4 top = raised, bottom = surface, edge = border;
        if (treatment == 1) { top = rgb(29, 163, 250); bottom = rgb(18, 94, 215); edge = rgb(71, 190, 255); }
        if (treatment == 2) { top = rgb(148, 122, 24); bottom = rgb(104, 85, 20); edge = gold; }
        if (treatment == 3) { top = rgb(75, 53, 67); bottom = rgb(54, 46, 61); edge = rgb(140, 80, 95); }
        if (treatment == 4) { top = rgb(31, 82, 133); bottom = rgb(26, 60, 100); edge = rgb(49, 132, 211); }
        if (hovered || ImGui::IsItemFocused()) {
            top.x = std::min(1.0f, top.x + .06f);
            top.y = std::min(1.0f, top.y + .06f);
            top.z = std::min(1.0f, top.z + .06f);
            edge = ink;
        }
        if (ImGui::IsItemActive()) std::swap(top, bottom);
        box(p, size, top, bottom, edge, 7);
        ImFont* font = ui::semiboldFont ? ui::semiboldFont : ImGui::GetFont();
        float textWidth = font->CalcTextSizeA(14 * s, FLT_MAX, 0, title).x;
        float iconWidth = glyph == ui::Icon::None ? 0 : 19 * s;
        float gap = iconWidth && *title ? 8 * s : 0;
        const float slack = (size.x - textWidth - iconWidth - gap) * .5f;
        float x = std::round(p.x + (*title ? (slack >= 0 ? slack : 4 * s) : (size.x - iconWidth) * .5f));
        if (iconWidth) ui::icon(glyph, ImVec2(x, p.y + (size.y - iconWidth) * .5f), iconWidth, color(ink));
        label(ImVec2(x + iconWidth + gap, p.y + (size.y - 14 * s) * .5f - s), title,
            size.x - (x - p.x) - iconWidth - gap - 6 * s, ink, 14, true);
        return pressed;
    }

    bool search(const char* id, const char* hint, char* value, size_t capacity, ImVec2 p, float width) const {
        box(p, ImVec2(width, 36 * s), raised, surface, border, 7);
        ui::icon(ui::Icon::Search, ImVec2(p.x + 11 * s, p.y + 9.5f * s), 17 * s, color(muted));
        ImGui::SetCursorScreenPos(ImVec2(p.x + 32 * s, p.y + (36 * s - ImGui::GetFrameHeight()) * .5f));
        ImGui::SetNextItemWidth(std::max(1.0f, width - 36 * s));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        bool changed = ImGui::InputTextWithHint(id, hint, value, capacity);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return changed;
    }

    void status(ImVec2 p, const std::string& value, bool available, float width) const {
        auto ink = available ? green : muted;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5 * s, p.y + 9 * s), 4.5f * s, color(ink));
        label(ImVec2(p.x + 20 * s, p.y), value, width - 20 * s, ink, 13, true);
    }
};

inline bool lightTheme() {
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return bg.x + bg.y + bg.z > 1.5f;
}

inline ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

inline ImVec4 withAlpha(ImVec4 value, float alpha) {
    value.w = alpha;
    return value;
}

enum class Tone { Secondary, Primary, Danger, Quiet };
enum class Accent { Info, Success, Warning, Danger };

inline ImVec4 accentColor(Accent accent) {
    const bool light = lightTheme();
    switch (accent) {
    case Accent::Success: return light ? rgb(0, 128, 90) : rgb(52, 211, 153);
    case Accent::Warning: return light ? rgb(158, 96, 0) : rgb(251, 191, 36);
    case Accent::Danger: return light ? rgb(196, 43, 58) : rgb(248, 113, 113);
    default: return light ? rgb(0, 106, 190) : rgb(96, 170, 255);
    }
}

// Gradient button that matches the panel buttons. Accepts the same label and
// size conventions as ImGui::Button, including "##id" suffixes and negative widths.
inline bool actionButton(const char* label, ImVec2 size = ImVec2(0, 0), Tone tone = Tone::Secondary) {
    const float s = scale();
    const bool light = lightTheme();
    PanelStyle theme(light);
    const char* hash = strstr(label, "##");
    const std::string visible = hash ? std::string(label, hash) : std::string(label);
    ImFont* font = semiboldFont ? semiboldFont : ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, visible.c_str());
    if (size.x == 0) size.x = textSize.x + 32 * s;
    else if (size.x < 0) size.x = std::max(4.0f, ImGui::GetContentRegionAvail().x + size.x);
    if (size.y == 0) size.y = ImGui::GetFrameHeight();

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    const ImVec4 white(1, 1, 1, 1);
    ImVec4 top = theme.raised, bottom = theme.surface, edge = theme.border, ink = theme.text;
    switch (tone) {
    case Tone::Primary: {
        top = mix(accent, white, 0.10f);
        bottom = mix(accent, ImVec4(0, 0, 0, 1), 0.22f);
        edge = mix(accent, white, 0.25f);
        const float brightness = accent.x * 0.299f + accent.y * 0.587f + accent.z * 0.114f;
        ink = brightness > 0.62f ? rgb(10, 18, 32) : white;
        break;
    }
    case Tone::Danger:
        top = light ? rgb(214, 66, 80) : rgb(128, 38, 52);
        bottom = light ? rgb(182, 44, 58) : rgb(92, 27, 39);
        edge = light ? rgb(170, 36, 50) : rgb(201, 80, 96);
        ink = light ? white : rgb(255, 222, 226);
        break;
    case Tone::Quiet:
        top = bottom = edge = ImVec4(0, 0, 0, 0);
        ink = theme.muted;
        break;
    default: break;
    }
    if (hovered) {
        if (tone == Tone::Quiet) {
            top = theme.raised;
            bottom = theme.raised;
            ink = theme.text;
        } else {
            top = mix(top, white, 0.07f);
            bottom = mix(bottom, white, 0.05f);
        }
        edge = tone == Tone::Secondary || tone == Tone::Quiet ? withAlpha(accent, 0.85f) : mix(edge, white, 0.15f);
    }
    if (held) std::swap(top, bottom);
    if (tone != Tone::Quiet || hovered) theme.box(p, size, top, bottom, edge, 7);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float textX = p.x + std::max(8 * s, (size.x - textSize.x) * 0.5f);
    const float textY = p.y + (size.y - textSize.y) * 0.5f;
    draw->PushClipRect(ImVec2(p.x + 4 * s, p.y), ImVec2(p.x + size.x - 4 * s, p.y + size.y), true);
    draw->AddText(font, fontSize, ImVec2(textX, textY), theme.color(ink), visible.c_str());
    draw->PopClipRect();
    return pressed;
}

// Modal dialog frame shared by all dialogs. Always pair a true result with endDialog().
inline bool beginDialog(const char* id, float width, bool autoResize = true, ImGuiWindowFlags extraFlags = 0,
    bool keepCentered = false) {
    const float s = scale();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), keepCentered ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (keepCentered) extraFlags |= ImGuiWindowFlags_NoMove;
    if (width > 0 && autoResize)
        ImGui::SetNextWindowSize(ImVec2(std::min(width * s, viewport->WorkSize.x * 0.92f), 0));
    const bool light = lightTheme();
    PanelStyle theme(light);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 22 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s, 10 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.01f, 0.02f, 0.04f, light ? 0.38f : 0.64f));
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | extraFlags;
    if (autoResize) flags |= ImGuiWindowFlags_AlwaysAutoResize;
    const bool open = ImGui::BeginPopupModal(id, nullptr, flags);
    if (!open) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
    }
    return open;
}

inline void endDialog() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

// Icon tile, title and optional subtitle. Returns true when the close button or Escape is used.
inline bool dialogHeader(Icon glyph, ImVec4 accent, const char* title, const std::string& subtitle = {},
    bool closable = false) {
    const float s = scale();
    PanelStyle theme(lightTheme());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float tile = 44 * s;
    const float closeSize = 30 * s;
    bool closed = false;

    if (closable) {
        const ImVec2 closePos(start.x + ImGui::GetContentRegionAvail().x - closeSize, start.y);
        ImGui::SetCursorScreenPos(closePos);
        closed = ImGui::InvisibleButton("##DialogClose", ImVec2(closeSize, closeSize));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            draw->AddRectFilled(closePos, ImVec2(closePos.x + closeSize, closePos.y + closeSize),
                theme.color(theme.raised), 8 * s);
        }
        icon(Icon::Close, ImVec2(closePos.x + 8 * s, closePos.y + 8 * s), 14 * s,
            theme.color(hovered ? theme.text : theme.muted));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            closed = true;
        ImGui::SetCursorScreenPos(start);
    }

    draw->AddRectFilled(start, ImVec2(start.x + tile, start.y + tile), theme.color(withAlpha(accent, 0.15f)), 12 * s);
    draw->AddRect(start, ImVec2(start.x + tile, start.y + tile), theme.color(withAlpha(accent, 0.38f)), 12 * s, 0, s);
    icon(glyph, ImVec2(start.x + 11 * s, start.y + 11 * s), 22 * s, theme.color(accent));
    ImGui::Dummy(ImVec2(tile, tile));
    ImGui::SameLine(0, 14 * s);

    const float textWidth = std::max(40 * s, ImGui::GetContentRegionAvail().x - (closable ? closeSize + 8 * s : 0));
    ImFont* headingFont = connectionHeadingFont ? connectionHeadingFont : semiboldFont;
    const float titleHeight = headingFont ? headingFont->FontSize : ImGui::GetTextLineHeight();
    float groupHeight = titleHeight;
    if (!subtitle.empty())
        groupHeight += 4 * s + ImGui::CalcTextSize(subtitle.c_str(), nullptr, false, textWidth).y;
    if (groupHeight < tile)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (tile - groupHeight) * 0.5f);

    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4 * s));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
    if (headingFont) ImGui::PushFont(headingFont);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (headingFont) ImGui::PopFont();
    if (!subtitle.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
        ImGui::TextUnformatted(subtitle.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 2 * s));
    return closed;
}

// Right-aligns the next row of footer buttons whose combined width is buttonsWidth.
inline void dialogFooter(float buttonsWidth) {
    const float s = scale();
    ImGui::Dummy(ImVec2(0, 4 * s));
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, available - buttonsWidth));
}

inline float footerButtonHeight() { return std::max(34 * scale(), ImGui::GetFrameHeight() + 6 * scale()); }

// Rounded status pill with a leading dot.
inline void statusChip(const char* text, ImVec4 ink) {
    const float s = scale();
    PanelStyle theme(lightTheme());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = textSize.y + 10 * s;
    const float width = textSize.x + 34 * s;
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height), theme.color(withAlpha(ink, 0.14f)), height * 0.5f);
    draw->AddRect(p, ImVec2(p.x + width, p.y + height), theme.color(withAlpha(ink, 0.35f)), height * 0.5f, 0, 1.0f);
    draw->AddCircleFilled(ImVec2(p.x + 13 * s, p.y + height * 0.5f), 4 * s, theme.color(ink));
    draw->AddText(ImVec2(p.x + 24 * s, p.y + 5 * s), theme.color(ink), text);
    ImGui::Dummy(ImVec2(width, height));
}

// Checkbox with a rounded box, accent fill and centered label. Same layout and
// ID rules as ImGui::Checkbox so it can replace it in place.
inline bool checkbox(const char* label, bool* value) {
    const float s = scale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* hash = strstr(label, "##");
    const std::string visible = hash ? std::string(label, hash) : std::string(label);
    const float frame = ImGui::GetFrameHeight();
    const ImVec2 textSize = ImGui::CalcTextSize(visible.c_str());
    const float width = frame + (visible.empty() ? 0.0f : style.ItemInnerSpacing.x + textSize.x);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, ImVec2(width, frame));
    if (pressed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    PanelStyle theme(lightTheme());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float box = std::round(std::clamp(ImGui::GetFontSize() + 2 * s, 14 * s, frame - 2 * s));
    const ImVec2 a(p.x + std::round((frame - box) * 0.5f), p.y + std::round((frame - box) * 0.5f));
    const ImVec2 b(a.x + box, a.y + box);
    const float rounding = 5 * s;
    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    if (*value) {
        ImVec4 glow = accent; glow.w = hovered ? 0.30f : 0.18f;
        draw->AddRectFilled(ImVec2(a.x - 2 * s, a.y - 2 * s), ImVec2(b.x + 2 * s, b.y + 2 * s), theme.color(glow), rounding + 2 * s);
        gradientRect(draw, a, b, shade(accent, 0.15f), shade(accent, -0.15f), rounding);
        const ImVec4 ink = brightness(accent) > 0.55f ? ImVec4(0.05f, 0.08f, 0.14f, 1) : ImVec4(1, 1, 1, 1);
        const float u = box / 20.0f;
        draw->PathLineTo(ImVec2(a.x + 5.0f * u, a.y + 10.5f * u));
        draw->PathLineTo(ImVec2(a.x + 8.5f * u, a.y + 14.0f * u));
        draw->PathLineTo(ImVec2(a.x + 15.0f * u, a.y + 6.5f * u));
        draw->PathStroke(theme.color(ink), 0, std::max(1.6f, 2.2f * u));
    } else {
        gradientRect(draw, a, b, theme.background, shade(theme.background, 0.04f), rounding);
        ImVec4 edge = hovered ? accent : theme.border;
        if (hovered) edge.w = 0.9f;
        draw->AddRect(a, b, theme.color(edge), rounding, 0, 1.4f * s);
    }
    if (!visible.empty())
        draw->AddText(ImVec2(p.x + frame + style.ItemInnerSpacing.x, p.y + (frame - textSize.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text), visible.c_str());
    return pressed;
}

// Combo box with a clean chevron instead of ImGui's square arrow button.
inline bool beginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const bool customArrow = (flags & ImGuiComboFlags_NoArrowButton) == 0;
    const bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
    if (customArrow) {
        const float size = std::round(std::min(14 * scale(), height * 0.5f));
        const ImU32 ink = ImGui::GetColorU32(open ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
        icon(open ? Icon::Up : Icon::Down, ImVec2(p.x + width - height * 0.5f - size * 0.5f, p.y + (height - size) * 0.5f), size, ink);
    }
    return open;
}

inline bool combo(const char* label, int* current, const char* const items[], int count) {
    const int index = std::clamp(*current, 0, std::max(0, count - 1));
    bool changed = false;
    if (beginCombo(label, count > 0 ? items[index] : "")) {
        for (int i = 0; i < count; ++i) {
            const bool selected = i == *current;
            if (ImGui::Selectable(items[i], selected) && !selected) { *current = i; changed = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Small uppercase caption used above grouped content.
inline void sectionLabel(const char* text) {
    PanelStyle theme(lightTheme());
    ImGui::PushStyleColor(ImGuiCol_Text, theme.muted);
    if (semiboldFont) ImGui::PushFont(semiboldFont);
    ImGui::TextUnformatted(text);
    if (semiboldFont) ImGui::PopFont();
    ImGui::PopStyleColor();
}

// Large clickable option with icon, title and description.
inline bool choiceCard(const char* id, Icon glyph, ImVec4 accent, const char* title, const char* description,
    float width = 0, const char* badge = nullptr) {
    const float s = scale();
    PanelStyle theme(lightTheme());
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    const float pad = 14 * s;
    const float iconBox = 38 * s;
    const float textX = pad + iconBox + 14 * s;
    const float textWidth = std::max(20 * s, width - textX - pad);
    ImFont* titleFont = semiboldFont ? semiboldFont : ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float descriptionHeight = description && *description
        ? ImGui::CalcTextSize(description, nullptr, false, textWidth).y : 0.0f;
    const float height = std::max(iconBox + pad * 2, pad * 2 + lineHeight + (descriptionHeight > 0 ? 4 * s + descriptionHeight : 0));

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImVec4 top = hovered ? mix(theme.raised, accent, 0.10f) : theme.raised;
    ImVec4 bottom = hovered ? mix(theme.surface, accent, 0.05f) : theme.surface;
    if (held) std::swap(top, bottom);
    const ImVec4 edge = hovered ? accent : (badge ? withAlpha(accent, 0.55f) : theme.border);
    theme.box(p, ImVec2(width, height), top, bottom, edge, 10);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 iconPos(p.x + pad, p.y + (height - iconBox) * 0.5f);
    draw->AddRectFilled(iconPos, ImVec2(iconPos.x + iconBox, iconPos.y + iconBox), theme.color(withAlpha(accent, 0.15f)), 10 * s);
    icon(glyph, ImVec2(iconPos.x + 9 * s, iconPos.y + 9 * s), 20 * s, theme.color(accent));

    const float contentHeight = lineHeight + (descriptionHeight > 0 ? 4 * s + descriptionHeight : 0);
    const float textY = p.y + (height - contentHeight) * 0.5f;
    draw->AddText(titleFont, fontSize, ImVec2(p.x + textX, textY), theme.color(theme.text), title);
    if (badge) {
        const float titleWidth = titleFont->CalcTextSizeA(fontSize, FLT_MAX, 0, title).x;
        const ImVec2 badgeText = ImGui::CalcTextSize(badge);
        const ImVec2 badgeMin(p.x + textX + titleWidth + 10 * s, textY - 1 * s);
        const ImVec2 badgeMax(badgeMin.x + badgeText.x + 14 * s, badgeMin.y + lineHeight + 2 * s);
        if (badgeMax.x < p.x + width - pad) {
            draw->AddRectFilled(badgeMin, badgeMax, theme.color(withAlpha(accent, 0.16f)), lineHeight);
            draw->AddText(ImVec2(badgeMin.x + 7 * s, badgeMin.y + 1 * s), theme.color(accent), badge);
        }
    }
    if (descriptionHeight > 0)
        draw->AddText(ImGui::GetFont(), fontSize, ImVec2(p.x + textX, textY + lineHeight + 4 * s),
            theme.color(theme.muted), description, nullptr, textWidth);
    return pressed;
}

// Slim rounded progress track with a gradient fill.
inline void progressTrack(ImVec2 p, float width, float height, float fraction, ImVec4 from, ImVec4 to) {
    const float s = scale();
    PanelStyle theme(lightTheme());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    const float radius = height * 0.5f;
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
        theme.color(lightTheme() ? rgb(222, 230, 241) : rgb(24, 35, 51)), radius);
    if (fraction <= 0.0f) return;
    const float fillWidth = std::max(height, width * fraction);
    const ImVec2 fillMax(p.x + fillWidth, p.y + height);
    const int first = draw->VtxBuffer.Size;
    draw->AddRectFilled(p, fillMax, theme.color(from), radius);
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        auto& vertex = draw->VtxBuffer[i];
        const float t = std::clamp((vertex.pos.x - p.x) / std::max(1.0f, fillWidth), 0.0f, 1.0f);
        const ImU32 alpha = vertex.col & IM_COL32_A_MASK;
        vertex.col = (theme.color(mix(from, to, t)) & ~IM_COL32_A_MASK) | alpha;
    }
    // Soft sheen that sweeps along the filled part while work is running.
    if (fraction < 1.0f && fillWidth > 40 * s) {
        const float sweep = std::fmod((float)ImGui::GetTime() * 0.6f, 1.0f);
        const float glowX = p.x + sweep * fillWidth;
        const float glowHalf = std::min(60 * s, fillWidth * 0.25f);
        draw->PushClipRect(p, fillMax, true);
        draw->AddRectFilledMultiColor(ImVec2(glowX - glowHalf, p.y), ImVec2(glowX, p.y + height),
            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 0));
        draw->AddRectFilledMultiColor(ImVec2(glowX, p.y), ImVec2(glowX + glowHalf, p.y + height),
            IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 46));
        draw->PopClipRect();
    }
}
}
