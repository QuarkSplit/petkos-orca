#include "PodBanner.hpp"

#include "libslic3r/Utils.hpp"

#include <wx/bitmap.h>
#include <wx/image.h>

#include <algorithm>
#include <map>

//The source strip bakes its vertical structure (gradient, sheen, edge shadow), so a
//banner of height h takes a CROP, never a scale: scaling would average the PEI grain
//away, and the grain is the point. The crop window is placed so the baked sheen band
//(row ~0.30 of the source) sits at ~0.35 of the banner.
static const wxImage &source_image()
{
    static wxImage img;
    static bool    tried = false;
    if (!tried) {
        tried = true;
        img.LoadFile(wxString::FromUTF8(Slic3r::var("gunmetal_pei.png").c_str()), wxBITMAP_TYPE_PNG);
    }
    return img;
}

static const wxBitmap *strip_for_height(int h)
{
    const wxImage &src = source_image();
    if (!src.IsOk() || h <= 0)
        return nullptr;

    static std::map<int, wxBitmap> cache;
    auto it = cache.find(h);
    if (it != cache.end())
        return &it->second;

    const int src_h  = src.GetHeight();
    const int crop_h = std::min(h, src_h);
    int start = int(0.30 * src_h - 0.35 * crop_h + 0.5);
    start = std::max(0, std::min(start, src_h - crop_h));

    wxImage window = src.GetSubImage(wxRect(0, start, src.GetWidth(), crop_h));
    //a banner taller than the source (does not happen at today's heights) stretches
    //rather than tiles vertically: grain distortion beats a visible horizontal seam
    if (crop_h < h)
        window = window.Scale(src.GetWidth(), h, wxIMAGE_QUALITY_BILINEAR);

    return &cache.emplace(h, wxBitmap(window)).first->second;
}

bool PodBanner::draw(wxDC &dc, const wxRect &rect, bool enabled, int phase_x)
{
    if (!enabled || rect.width <= 0 || rect.height <= 0)
        return false;
    const wxBitmap *strip = strip_for_height(rect.height);
    if (strip == nullptr)
        return false;

    const int tile_w = strip->GetWidth();
    //no DC clipping here: wxDC::DestroyClippingRegion would also drop a clip the
    //caller owns, so the final partial tile is cropped as a sub-bitmap instead.
    //phase_x is the caller's own x inside the textured parent, so a pixel at
    //parent-x = phase_x + rect.x reads the same tile column the parent painted there.
    int offset = (rect.x + phase_x) % tile_w;
    if (offset < 0)
        offset += tile_w;

    int x = rect.x;
    while (x < rect.x + rect.width) {
        const int avail = rect.x + rect.width - x;
        const int piece = std::min(tile_w - offset, avail);
        if (offset == 0 && piece == tile_w)
            dc.DrawBitmap(*strip, x, rect.y);
        else
            dc.DrawBitmap(strip->GetSubBitmap(wxRect(offset, 0, piece, rect.height)), x, rect.y);
        x += piece;
        offset = 0;
    }
    return true;
}
