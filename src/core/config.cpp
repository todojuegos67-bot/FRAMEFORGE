#include "core/config.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace ff {
static void trim(char* s) {
    char* e = s + std::strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

bool loadSettings(const std::wstring& path, Settings& s) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == ';' || *p == '#' || *p == '[') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = 0; char* k = p; char* v = eq + 1; trim(k);
        while (*v == ' ') ++v;
        int iv = std::atoi(v); float fv = (float)std::atof(v);
        if (!std::strcmp(k, "multiplier")) s.multiplier = iv;
        else if (!std::strcmp(k, "upscale")) s.upscale = iv;
        else if (!std::strcmp(k, "scale")) s.scale = iv;
        else if (!std::strcmp(k, "sharpness")) s.sharpness = fv;
        else if (!std::strcmp(k, "flow_downscale")) s.flowDownscale = iv;
        else if (!std::strcmp(k, "vrr")) s.vrr = iv;
        else if (!std::strcmp(k, "debug_view")) s.debugView = iv;
        else if (!std::strcmp(k, "hud_mask")) s.hudMask = iv;
        else if (!std::strcmp(k, "hud_frames")) s.hudFrames = iv;
        else if (!std::strcmp(k, "language")) s.lang = iv;
    }
    fclose(f);
    s.multiplier = std::clamp(s.multiplier, 1, 4);
    s.upscale = std::clamp(s.upscale, 0, 2);
    s.scale = std::clamp(s.scale, 0, 3);
    s.sharpness = std::clamp(s.sharpness, 0.0f, 1.0f);
    s.flowDownscale = std::clamp(s.flowDownscale, 1, 4);
    s.debugView = std::clamp(s.debugView, 0, 3);
    s.hudFrames = std::clamp(s.hudFrames, 1, 30);
    s.lang = std::clamp(s.lang, 0, 1);
    return true;
}

bool saveSettings(const std::wstring& path, const Settings& s) {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return false;
    fprintf(f, "; FrameForge\n[frameforge]\n");
    fprintf(f, "; 1..4 (1 = sin generacion / pass-through)\nmultiplier=%d\n", s.multiplier);
    fprintf(f, "; 0 = sin escalado, 1 = FSR1 (EASU+RCAS), 2 = bilineal\nupscale=%d\n", s.upscale);
    fprintf(f, "; 0 = ajustar a pantalla, 1 = 1.5x, 2 = 2x, 3 = 3x\nscale=%d\n", s.scale);
    fprintf(f, "; nitidez RCAS 0..1\nsharpness=%.3f\n", s.sharpness);
    fprintf(f, "; divisor de resolucion del flujo optico 1..4\nflow_downscale=%d\n", s.flowDownscale);
    fprintf(f, "; permitir tearing (VRR) si el sistema lo soporta\nvrr=%d\n", s.vrr);
    fprintf(f, "; 0 nada, 1 flujo, 2 oclusion, 3 HUD\ndebug_view=%d\n", s.debugView);
    fprintf(f, "hud_mask=%d\nhud_frames=%d\n", s.hudMask, s.hudFrames);
    fprintf(f, "; 0 = espanol, 1 = english\nlanguage=%d\n", s.lang);
    fclose(f);
    return true;
}
}
