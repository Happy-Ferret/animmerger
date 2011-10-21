#include <gd.h>
#include <cstdio>
#include <cmath>
#include <iostream>

#include "openmp.hh"
#include "canvas.hh"
#include "align.hh"
#include "palette.hh"
#include "dither.hh"
#include "quantize.hh"
#include "fparser.hh"
#include "averages.hh"

#ifdef _OPENMP
#include <omp.h>
#endif

bool CGA16mode = false;

int pad_top=0, pad_bottom=0, pad_left=0, pad_right=0;

int SaveGif = -1;
bool UseDitherCache = true;
std::string OutputNameTemplate = "%2$s-%1$04u.%3$s";

class CanvasFunctionParser: public FunctionParser
{
public:
    static double MakeRGB(const double* vars)
    {
        int r = vars[0], g = vars[1], b = vars[2];
        int l = r*299 + g*587 + b*114;
        if(l <= 0)        return double( 0x000000 );
        if(l >= 1000*255) return double( 0xFFFFFF );
        double ll = l / 255e3, ss = 1.0;
        for(unsigned n=0; n<3; ++n)
        {
            if(vars[n] > 255.0)    ss = std::min(ss, (ll-1.0) / (ll - vars[n]/255.0));
            else if(vars[n] < 0.0) ss = std::min(ss,     ll   / (ll - vars[n]/255.0));
        }
        if(ss != 1.0)
        {
            r = ((r/255.0 - ll) * ss + ll) * 255;
            g = ((g/255.0 - ll) * ss + ll) * 255;
            b = ((b/255.0 - ll) * ss + ll) * 255;
        }
        return double( (r<<16) + (g<<8) + (b) );
    }
    CanvasFunctionParser()
    {
        AddConstant("pi",  M_PI);
        AddConstant("e",   M_E);
        AddFunction("make_rgb", MakeRGB, 3);
    }
};

static CanvasFunctionParser transformation_parser;
std::string transform_common;
std::string transform_r = "r";
std::string transform_g = "g";
std::string transform_b = "b";
bool UsingTransformations = false;
bool TransformationDependsOnX       = false;
bool TransformationDependsOnY       = false;
bool TransformationDependsOnFrameNo = false;
bool TransformationGsameAsR = false;
bool TransformationBsameAsR = false;
bool TransformationBsameAsG = false;

void SetColorTransformations()
{
    // First, for diagnostics, parse each color component separately
    {
        std::string tmp_r = transform_common + transform_r;
        std::string tmp_g = transform_common + transform_g;
        std::string tmp_b = transform_common + transform_b;
        CanvasFunctionParser parser_r;
        CanvasFunctionParser parser_g;
        CanvasFunctionParser parser_b;
        int r_error = parser_r.Parse(tmp_r.c_str(), "r,g,b,frameno,x,y");
        int g_error = parser_g.Parse(tmp_g.c_str(), "r,g,b,frameno,x,y");
        int b_error = parser_b.Parse(tmp_b.c_str(), "r,g,b,frameno,x,y");
        if(r_error >= 0 || g_error >= 0 || b_error >= 0)
        {
            if(r_error >= 0)
                std::fprintf(stderr, "Parse error (%s) in red color formula:\n%s\n%*s\n",
                    parser_r.ErrorMsg(), tmp_r.c_str(), r_error+1, "^");
            if(g_error >= 0)
                std::fprintf(stderr, "Parse error (%s) in green color formula:\n%s\n%*s\n",
                    parser_g.ErrorMsg(), tmp_g.c_str(), g_error+1, "^");
            if(b_error >= 0)
                std::fprintf(stderr, "Parse error (%s) in blue color formula:\n%s\n%*s\n",
                    parser_b.ErrorMsg(), tmp_b.c_str(), b_error+1, "^");
            return;
        }
    }

    TransformationGsameAsR = transform_g == transform_r;
    TransformationBsameAsR = transform_b == transform_r;
    TransformationBsameAsG = transform_b == transform_g;
    // Then produce an optimized parser that produces all components at once
    std::string merged = transform_common;
    std::string r_expr = transform_r;
    std::string g_expr = transform_g;
    std::string b_expr = transform_b;
    if(TransformationGsameAsR || TransformationBsameAsR)
    {
        merged += "animmerger_R:=(" + r_expr + ");";
        r_expr = "animmerger_R";
    }
    if(TransformationGsameAsR)
        g_expr = "animmerger_R";
    else if(TransformationBsameAsG)
    {
        merged += "animmerger_G:=(" + g_expr + ");";
        g_expr = "animmerger_G";
    }
    if(TransformationBsameAsR)
        b_expr = "animmerger_R";
    else if(TransformationBsameAsG)
        b_expr = "animmerger_G";
    merged += "make_rgb(" + r_expr + "," + g_expr + "," + b_expr + ")";

    int error = transformation_parser.Parse(merged.c_str(), "r,g,b,frameno,x,y");
    if(error >= 0)
    {
        if(error >= 0)
            std::fprintf(stderr, "Parse error (%s) in color formula:\n%s\n%*s\n",
                transformation_parser.ErrorMsg(), merged.c_str(), error+1, "^");
        return;
    }

    UsingTransformations = transform_r != "r"
                        || transform_g != "g"
                        || transform_b != "b";

    if(UsingTransformations)
    {
        if(verbose >= 1)
        {
            std::printf("Merged color transformation formula: %s\n", merged.c_str());
            if(verbose >= 3)
            {
                std::printf("Bytecode before optimization:\n");
                std::fflush(stdout);
                transformation_parser.PrintByteCode(std::cout);
                std::cout << std::flush;
            }
        }
        transformation_parser.Optimize();
        transformation_parser.Optimize();
        if(verbose >= 3)
        {
            std::printf("Bytecode after optimization:\n");
            std::fflush(stdout);
            transformation_parser.PrintByteCode(std::cout);
            std::cout << std::flush;
        }
    }

    TransformationDependsOnX       = false;
    TransformationDependsOnY       = false;
    TransformationDependsOnFrameNo = false;
    if(UsingTransformations)
    {
        if(CanvasFunctionParser().Parse(merged.c_str(), "r,g,b,frameno,y") >= 0)
            TransformationDependsOnX = true;
        if(CanvasFunctionParser().Parse(merged.c_str(), "r,g,b,frameno,x") >= 0)
            TransformationDependsOnY = true;
        if(CanvasFunctionParser().Parse(merged.c_str(), "r,g,b,x,y") >= 0)
            TransformationDependsOnFrameNo = true;

        if(verbose >= 2)
        {
            std::printf(" - Found out that it %s on the X coordinate\n",
                TransformationDependsOnX ? "depends":"doesn't depend"
                );
            std::printf(" - Found out that it %s on the Y coordinate\n",
                TransformationDependsOnY ? "depends":"doesn't depend"
                );
            std::printf(" - Found out that it %s on the frame number\n",
                TransformationDependsOnFrameNo ? "depends":"doesn't depend"
                );
        }
    }
}
inline double TransformColor(int r, int g, int b, unsigned frameno,unsigned x,unsigned y)
{
    double vars[6] =
    {
        double(r), double(g), double(b),
        double(frameno), double(x), double(y)
    };
    return transformation_parser.Eval(vars);
}
void TransformColor(uint32& pix, unsigned frameno,unsigned x,unsigned y)
{
    int r = (pix >> 16) & 0xFF, g = (pix >> 8) & 0xFF, b = (pix & 0xFF);
    double pix_dbl = TransformColor(r,g,b, frameno,x,y);
    pix = (pix & 0xFF000000u) | ((unsigned) pix_dbl);
}

static inline bool veq
    (const VecType<uint32>& a,
     const VecType<uint32>& b) // For ChangeLog
{
    if(a.size() != b.size()) return false;
    return std::memcmp(&a[0], &b[0], a.size() * sizeof(uint32)) == 0;
}

const VecType<uint32>
TILE_Tracker::LoadScreen(int ox,int oy, unsigned sx,unsigned sy,
                         unsigned timer,
                         PixelMethod method) const
{
    // Create the result vector filled with default pixel value
    VecType<uint32> result(sy*sx, DefaultPixel);

    const int xbegin = ox;
    const int xend   = ox+sx-1;

    const int xscreen_begin = xbegin/256;
    const int xscreen_end   = xend  /256;

    const int ybegin = oy;
    const int yend   = oy+sy-1;

    const int yscreen_begin = ybegin/256;
    const int yscreen_end   = yend  /256;

/*
    std::fprintf(stderr, "Loading screens x(%d..%d)y(%d..%d)\n",
        xscreen_begin,xscreen_end,
        yscreen_begin,yscreen_end);
*/

    // Load each cube that falls into the requested region

    unsigned targetpos=0;
    unsigned this_cube_ystart = oy&255;
    for(int yscreen=yscreen_begin; yscreen<=yscreen_end; ++yscreen)
    {
        unsigned this_cube_yend = yscreen==yscreen_end ? ((oy+sy-1)&255) : 255;
        unsigned this_cube_ysize = (this_cube_yend-this_cube_ystart)+1;

        ymaptype::const_iterator yi = screens.find(yscreen);
        if(yi != screens.end())
        {
            const xmaptype& xmap = yi->second;

            unsigned this_cube_xstart = ox&255;
            for(int xscreen=xscreen_begin; xscreen<=xscreen_end; ++xscreen)
            {
                unsigned this_cube_xend = xscreen==xscreen_end ? ((ox+sx-1)&255) : 255;
                unsigned this_cube_xsize = (this_cube_xend-this_cube_xstart)+1;
    /*
                std::fprintf(stderr, " Cube(%u,%u)-(%u,%u)\n",
                    this_cube_xstart,this_cube_xend,
                    this_cube_ystart,this_cube_yend);
    */
                xmaptype::const_iterator xi = xmap.find(xscreen);
                if(xi != xmap.end())
                {
                    const cubetype& cube = xi->second;
                    /* If this screen is not yet initialized, we'll skip over
                     * it, since there's no real reason to initialize it at
                     * this point. */

                    cube.pixels->GetLiveSectionInto(
                        method,timer,
                        &result[targetpos], sx,
                        this_cube_xstart,
                        this_cube_ystart,
                        this_cube_xsize,
                        this_cube_ysize);
                }

                targetpos+= this_cube_xsize;

                this_cube_xstart=0;
            }
            targetpos += sx * (this_cube_ysize-1);
        }
        else
            targetpos += sx * this_cube_ysize;

        this_cube_ystart=0;
    }

    return result;
}

const VecType<uint32>
TILE_Tracker::LoadBackground(int ox,int oy, unsigned sx,unsigned sy) const
{
    // Create the result vector filled with default pixel value
    VecType<uint32> result(sy*sx, DefaultPixel);

    const int xbegin = ox;
    const int xend   = ox+sx-1;

    const int xscreen_begin = xbegin/256;
    const int xscreen_end   = xend  /256;

    const int ybegin = oy;
    const int yend   = oy+sy-1;

    const int yscreen_begin = ybegin/256;
    const int yscreen_end   = yend  /256;

/*
    std::fprintf(stderr, "Loading screens x(%d..%d)y(%d..%d)\n",
        xscreen_begin,xscreen_end,
        yscreen_begin,yscreen_end);
*/

    // Load each cube that falls into the requested region

    unsigned targetpos=0;
    unsigned this_cube_ystart = oy&255;
    for(int yscreen=yscreen_begin; yscreen<=yscreen_end; ++yscreen)
    {
        unsigned this_cube_yend = yscreen==yscreen_end ? ((oy+sy-1)&255) : 255;
        unsigned this_cube_ysize = (this_cube_yend-this_cube_ystart)+1;

        ymaptype::const_iterator yi = screens.find(yscreen);
        if(yi != screens.end())
        {
            const xmaptype& xmap = yi->second;

            unsigned this_cube_xstart = ox&255;
            for(int xscreen=xscreen_begin; xscreen<=xscreen_end; ++xscreen)
            {
                unsigned this_cube_xend = xscreen==xscreen_end ? ((ox+sx-1)&255) : 255;
                unsigned this_cube_xsize = (this_cube_xend-this_cube_xstart)+1;
    /*
                std::fprintf(stderr, " Cube(%u,%u)-(%u,%u)\n",
                    this_cube_xstart,this_cube_xend,
                    this_cube_ystart,this_cube_yend);
    */
                xmaptype::const_iterator xi = xmap.find(xscreen);
                if(xi != xmap.end())
                {
                    const cubetype& cube = xi->second;
                    /* If this screen is not yet initialized, we'll skip over
                     * it, since there's no real reason to initialize it at
                     * this point. */

                    cube.pixels->GetStaticSectionInto(
                        &result[targetpos], sx,
                        this_cube_xstart,
                        this_cube_ystart,
                        this_cube_xsize,
                        this_cube_ysize);
                }

                targetpos+= this_cube_xsize;

                this_cube_xstart=0;
            }
            targetpos += sx * (this_cube_ysize-1);
        }
        else
            targetpos += sx * this_cube_ysize;

        this_cube_ystart=0;
    }

    return result;
}

void
TILE_Tracker::PutScreen
    (const uint32*const input, int ox,int oy, unsigned sx,unsigned sy,
     unsigned timer)
{
    /* Nearly the same as LoadScreen. */

    const int xbegin = ox;
    const int xend   = ox+sx-1;

    const int xscreen_begin = xbegin/256;
    const int xscreen_end   = xend  /256;

    const int ybegin = oy;
    const int yend   = oy+sy-1;

    const int yscreen_begin = ybegin/256;
    const int yscreen_end   = yend  /256;

/*
    std::fprintf(stderr, "Writing screens x(%d..%d)y(%d..%d)\n",
        xscreen_begin,xscreen_end,
        yscreen_begin,yscreen_end);
*/
    unsigned targetpos=0;
    unsigned this_cube_ystart = oy&255;
    for(int yscreen=yscreen_begin; yscreen<=yscreen_end; ++yscreen)
    {
        xmaptype& xmap = screens[yscreen];

        unsigned this_cube_yend = yscreen==yscreen_end ? ((oy+sy-1)&255) : 255;
        unsigned this_cube_ysize = (this_cube_yend-this_cube_ystart)+1;

        unsigned this_cube_xstart = ox&255;
        for(int xscreen=xscreen_begin; xscreen<=xscreen_end; ++xscreen)
        {
            unsigned this_cube_xend = xscreen==xscreen_end ? ((ox+sx-1)&255) : 255;
            unsigned this_cube_xsize = (this_cube_xend-this_cube_xstart)+1;

            cubetype& cube = xmap[xscreen];

            /* If this screen is not yet initialized, we'll initialize it */
            if(cube.pixels.empty())
            {
                //cube.pixels.resize(256*256);
                cube.pixels.init();
            }
            cube.changed = true;

/*
            std::fprintf(stderr, " Cube(%u,%u)-(%u,%u)\n",
                this_cube_xstart,this_cube_xend,
                this_cube_ystart,this_cube_yend);
*/
            cube.pixels->PutSectionInto(
                timer,
                &input[targetpos], sx,
                this_cube_xstart,
                this_cube_ystart,
                this_cube_xsize,
                this_cube_ysize);

            targetpos+= this_cube_xsize;

            this_cube_xstart=0;
        }

        targetpos += sx * (this_cube_ysize-1);

        this_cube_ystart=0;
    }
}

bool TILE_Tracker::IsHeavyDithering(bool animated) const
{
    if(!PaletteReductionMethod.empty() && !(SaveGif == -1 && !animated))
    {
        // Will use dithering engine, so check if the dithering
        // will incur a significant load. If it does, do threading
        // per scanline rather than by frame.
        if(DitherColorListSize > 1
        && DitherErrorFactor > 0.0
        && DitherColorListSize *
             (CurrentPalette.Size() + CurrentPalette.NumCombinations())
           > 1000
          )
        {
            return true;
        }
    }
    return false;
}

void TILE_Tracker::Save(unsigned method)
{
    if(CurrentTimer == 0)
        return;

    if(method == ~0u)
    {
        for(unsigned m=0; m<NPixelMethods; ++m)
        {
            if(pixelmethods_result & (1ul << m))
                Save( (PixelMethod) m);
        }
        return;
    }

    const bool animated = (1ul << method) & AnimatedPixelMethodsMask;

    std::fprintf(stderr, "Saving(%d)\n", CurrentTimer);

    if(!PaletteReductionMethod.empty())
    {
        DitheringMatrix = CreateDispersedDitheringMatrix();
        TemporalMatrix = CreateTemporalDitheringMatrix();
    }

#ifdef _OPENMP
    omp_set_nested(1);
#endif

    if(animated)
    {
        unsigned SavedTimer = CurrentTimer;

        if((1ul << method) & LoopingPixelMethodsMask)
        {
            if(SavedTimer >= LoopingLogLength)
                SavedTimer = LoopingLogLength;
        }

        if(!PaletteReductionMethod.empty())
        {
            CreatePalette( (PixelMethod) method, SavedTimer );
        }

        for(unsigned frame=0; frame<SavedTimer; frame+=1)
        //for(unsigned frame=5200; frame<SavedTimer; frame+=1)
        {
            //std::fprintf(stderr, "Saving frame %u/%u @ %u\n",
            //    frame, SavedTimer, SequenceBegin);
            SaveFrame( (PixelMethod)method, frame, SequenceBegin + frame);
        }
        fflush(stdout);
    }
    else
    {
        if(!PaletteReductionMethod.empty())
        {
            CreatePalette( (PixelMethod) method, 1 );
        }

        for(unsigned dummy=0; dummy<1; ++dummy)
            SaveFrame( (PixelMethod)method, 0, SequenceBegin);
    }
}

template<bool TransformColors>
HistogramType TILE_Tracker::CountColors(PixelMethod method, unsigned nframes)
{
    HistogramType Histogram;

    // Create a histogram of the used colors, unless it's going
    // to be replaced immediately thereafter.
    if(PaletteReductionMethod.empty()
    || PaletteReductionMethod.front().entries.empty())
    {
        const int ymi = ymin, yma = ymax;
        const int xmi = xmin, xma = xmax;
        const unsigned wid = xma-xmi;
        const unsigned hei = yma-ymi;
        std::fprintf(stderr, "Counting colors... (%u frames)\n", nframes);
        VecType<uint32> prev_frame;
        for(unsigned frameno=0; frameno<nframes; frameno+=1)
        {
            /*if(frameno == 20)
                frameno = nframes*6/16;
            else if(frameno == nframes*9/16)
                frameno = nframes*999/1000;*/

            std::fprintf(stderr, "\rFrame %u/%u, %u so far...",
                frameno+1, nframes, (unsigned) Histogram.size());
            std::fflush(stderr);
          #if 1
            /* Only count histogram from content that
             * changes between previous and current frame
             */
            VecType<uint32> frame ( LoadScreen(xmi,ymi, wid,hei, frameno, method) );
            unsigned a=0;
            for(; a < prev_frame.size() && a < frame.size(); ++a)
            {
                if(frame[a] != prev_frame[a])
                {
                    uint32 p = prev_frame[a], q = frame[a];
                    if(TransformColors)
                    {
                        TransformColor(p, frameno, a/256, a%256);
                        TransformColor(q, frameno, a/256, a%256);
                    }
                    ++Histogram[p];
                    ++Histogram[q];
                }
            }
            for(; a < frame.size(); ++a)
            {
                uint32 p = frame[a];
                if(TransformColors)
                {
                    TransformColor(p, frameno,a/256, a%256);
                }
                ++Histogram[p];
            }
            prev_frame.swap(frame);
          #else
            for(ymaptype::const_iterator
                yi = screens.begin();
                yi != screens.end();
                ++yi)
            for(xmaptype::const_iterator
                xi = yi->second.begin();
                xi != yi->second.end();
                ++xi)
            {
                const cubetype& cube      = xi->second;
                uint32 result[256*256];
                cube.pixels->GetLiveSectionInto(method, frameno, result,256, 0,0, 256,256);
                for(unsigned a=0; a<256*256; ++a)
                {
                    uint32 p = result[a];
                    if(TransformColors)
                    {
                        TransformColor(p, frameno, a/256, a%256);
                    }
                    ++Histogram[p];
                }
            }
          #endif
        }
        // Reduce the histogram into a usable palette
        std::fprintf(stderr, "\n%u colors detected\n",(unsigned) Histogram.size());
    }
    return Histogram;
}

void TILE_Tracker::CreatePalette(PixelMethod method, unsigned nframes)
{
    HistogramType Histogram = UsingTransformations
        ? CountColors<true>(method, nframes)
        : CountColors<false>(method, nframes);
    ReduceHistogram(Histogram);

    const bool animated = (1ul << method) & AnimatedPixelMethodsMask;
    unsigned limit = Histogram.size();
    if(SaveGif == 1 || (SaveGif == -1 && animated)) limit = 256;
    CurrentPalette = MakePalette(Histogram, limit);
}

void TILE_Tracker::SaveFrame(PixelMethod method, unsigned frameno, unsigned img_counter)
{
    const bool animated = (1ul << method) & AnimatedPixelMethodsMask;

    const int ymi = ymin, yma = ymax;
    const int xmi = xmin, xma = xmax;

    unsigned wid = xma-xmi;
    unsigned hei = yma-ymi;

    if(wid <= 1 || hei <= 1) return;

    VecType<uint32> screen ( LoadScreen(xmi,ymi, wid,hei, frameno, method) );

    const char* methodnamepiece = "tile";
    if(pixelmethods_result != (1ul << method))
    {
        // Multi-method output
        #define MakePixName(o,f,name) #name,
        static const char* const Templates[NPixelMethods] =
        {
             DefinePixelMethods(MakePixName)
        };
        #undef MakePixName
        methodnamepiece = Templates[method];
    }

    bool MakeGif  = SaveGif == 1 || (SaveGif == -1 && animated);
    bool Dithered = !PaletteReductionMethod.empty();

    char Filename[512] = {0}; // explicit init keeps valgrind happy
    std::snprintf(Filename, sizeof(Filename),
        OutputNameTemplate.c_str(),
        img_counter,
        methodnamepiece,
        MakeGif ? "gif" : "png");
    Filename[sizeof(Filename)-1] = '\0';

    std::fprintf(stderr, "%s: (%d,%d)-(%d,%d)\n", Filename, 0,0, xma-xmi, yma-ymi);
    std::fflush(stderr);

    bool was_identical = false;

    if(TemporalDitherSize == 1 && animated && !UsingTransformations)
    {
        if(veq(screen, LastScreen) && !LastFilename.empty())
        {
            std::fprintf(stderr, "->link (%u,%u)\n",
                (unsigned)screen.size(),
                (unsigned)LastScreen.size());

            std::string cmd = "ln -f "+LastFilename+" "+Filename;
            system(cmd.c_str());

            was_identical = true;
        }
        LastScreen   = screen;
        LastFilename = Filename;
    }
    if(was_identical) return;

    gdImagePtr im;
    if(Dithered)
    {
        if(Diffusion == Diffusion_None)
        {
            if(!DitheringSections.empty())
              if(UsingTransformations)
                im = CreateFrame_Palette_Dither_Sections<true,false>(screen, frameno, wid, hei);
              else
                im = CreateFrame_Palette_Dither_Sections<false,false>(screen, frameno, wid, hei);
            else
              if(UsingTransformations)
                im = CreateFrame_Palette_Dither<true,false>(screen, frameno, wid, hei);
              else
                im = CreateFrame_Palette_Dither<false,false>(screen, frameno, wid, hei);
        }
        else
        {
            if(!DitheringSections.empty())
              if(UsingTransformations)
                im = CreateFrame_Palette_Dither_Sections<true,true>(screen, frameno, wid, hei);
              else
                im = CreateFrame_Palette_Dither_Sections<false,true>(screen, frameno, wid, hei);
            else
              if(UsingTransformations)
                im = CreateFrame_Palette_Dither<true,true>(screen, frameno, wid, hei);
              else
                im = CreateFrame_Palette_Dither<false,true>(screen, frameno, wid, hei);
        }
    }
    else
    {
        if(MakeGif)
          if(UsingTransformations)
            im = CreateFrame_Palette_Auto<true>(screen, frameno, wid, hei);
          else
            im = CreateFrame_Palette_Auto<false>(screen, frameno, wid, hei);
        else
          if(UsingTransformations)
            im = CreateFrame_TrueColor<true>(screen, frameno, wid, hei);
          else
            im = CreateFrame_TrueColor<false>(screen, frameno, wid, hei);
    }

    ImgResult imgdata;
    if(MakeGif && gdImageTrueColor(im))
        gdImageTrueColorToPalette(im, false, 256);

    imgdata.first = MakeGif
        ? gdImageGifPtr(im, &imgdata.second)
        : gdImagePngPtrEx(im, &imgdata.second, 1);
    gdImageDestroy(im);

    if(imgdata.first)
    {
        FILE* fp = fopen(Filename, "wb");
        if(!fp)
            std::perror(Filename);
        else
        {
            std::fwrite(imgdata.first, 1, imgdata.second, fp);
            std::fclose(fp);
        }
        gdFree(imgdata.first);
    }
}

class transform_cache_t: public std::map<uint32,uint32, std::less<uint32>, FSBAllocator<int> >
{
    typedef std::map<uint32,uint32, std::less<uint32>, FSBAllocator<int> > parent;
public:
    using parent::iterator;
    using parent::const_iterator;
};
class transform_caches_t: public std::map<unsigned,transform_cache_t, std::less<unsigned>, FSBAllocator<int> >
{
    typedef std::map<unsigned,transform_cache_t, std::less<unsigned>, FSBAllocator<int> > parent;
public:
    using parent::iterator;
    using parent::const_iterator;
};

static inline transform_caches_t& GetTransformCache()
{
  #ifdef _OPENMP
    static std::vector<transform_caches_t> transform_caches( omp_get_num_procs()*2 );
    transform_caches_t& transform_cache = transform_caches[omp_get_thread_num()];
  #else
    static transform_caches_t transform_cache;
  #endif
    return transform_cache;
}


class dither_cache_t: public std::map<uint32, MixingPlan, std::less<uint32>, FSBAllocator<int> >
{
    typedef std::map<uint32, MixingPlan, std::less<uint32>, FSBAllocator<int> > parent;
public:
    using parent::iterator;
    using parent::const_iterator;
};

static inline dither_cache_t& GetDitherCache(size_t n, size_t of_n)
{
  #ifdef _OPENMP
    size_t limit = 2*omp_get_num_procs(), cur = omp_get_thread_num();
    static omp_lock_t lock;
  #else
    size_t limit = 1, cur = 0;
  #endif
    static std::map<size_t/*of_n*/, std::vector<dither_cache_t> > dither_caches;
  #ifdef _OPENMP
    omp_set_lock(&lock);
  #endif
    std::map<size_t, std::vector<dither_cache_t> >::iterator
        i = dither_caches.lower_bound(of_n);
    if(i == dither_caches.end() || i->first != of_n)
    {
        i = dither_caches.insert(i,
            {of_n, std::vector<dither_cache_t>(of_n * limit)}
                                );
    }
  #ifdef _OPENMP
    omp_unset_lock(&lock);
  #endif
    return i->second[n + of_n * omp_get_thread_num()];
}

static inline uint32 DoCachedPixelTransform(
    transform_caches_t& transform_caches,
    uint32 pix, unsigned wid,unsigned hei,
    unsigned frameno,unsigned x,unsigned y)
{
    unsigned profile = 0, profilemax = 1;
    if(TransformationDependsOnX)       {profile += x*profilemax; profilemax*=wid; }
    if(TransformationDependsOnY)       {profile += y*profilemax; profilemax*=hei; }
    if(TransformationDependsOnFrameNo) {profile += frameno*profilemax; }

    transform_cache_t& cachepos = transform_caches[profile];
    transform_cache_t::iterator i = cachepos.lower_bound(pix);
    if(i == cachepos.end() || i->first != pix)
    {
        uint32 outpix = pix;
        TransformColor(outpix, frameno,x,y);
        cachepos.insert(i, std::make_pair(pix, outpix));
        return outpix;
    }
    return i->second;
}

template<bool TransformColors>
inline unsigned TILE_Tracker::GetMixColor
   (dither_cache_t& cache,
    transform_caches_t& transform_caches,
    unsigned wid,unsigned hei, unsigned frameno,
    unsigned x,unsigned y, uint32 pix,
    const Palette& pal)
{
    if(pix == DefaultPixel) pix = 0x7F000000u;
    if(TransformColors) pix = DoCachedPixelTransform(transform_caches, pix,wid,hei, frameno,x,y);
    ColorInfo input(pix);

    // Find two closest entries from palette and use o8x8 dithering
    MixingPlan output;
    if(UseDitherCache)
    {
        dither_cache_t::iterator i = cache.lower_bound(pix);
        if(i == cache.end() || i->first != pix)
        {
            output = FindBestMixingPlan(input, pal);
            cache.insert(i, std::make_pair(pix, output));
        }
        else
            output = i->second;
    }
    else
    {
        output = FindBestMixingPlan(input, pal);
    }

    unsigned pattern_value =
        DitheringMatrix
            [ ((y%DitherMatrixHeight)*DitherMatrixWidth
             + (x%DitherMatrixWidth)
               )// % (DitherMatrixHeight*DitherMatrixWidth)
            ];
    const unsigned max_pattern_value = DitherMatrixWidth * DitherMatrixHeight;
    return output[ pattern_value * output.size() / max_pattern_value ];
}


template<bool TransformColors>
gdImagePtr TILE_Tracker::CreateFrame_TrueColor(
    const VecType<uint32>& screen,
    unsigned frameno, unsigned wid, unsigned hei)
{
    gdImagePtr im = gdImageCreateTrueColor(wid+pad_left+pad_right, hei+pad_top+pad_bottom);
    gdImageAlphaBlending(im, 0);
    gdImageSaveAlpha(im,     1);

    #pragma omp parallel for schedule(static)
    for(unsigned y=0; y<hei; ++y)
    {
        transform_caches_t& transform_cache = GetTransformCache();

        for(unsigned p=y*wid, x=0; x<wid; ++x)
        {
            uint32 pix = screen[p+x];
            if(pix == DefaultPixel)
                pix = 0x7F000000u;
            if(TransformColors)
                pix = DoCachedPixelTransform(transform_cache, pix,wid,hei, frameno,x,y);
            gdImageSetPixel(im, x+pad_left,y+pad_top, pix);
        }
    }
    return im;
}

template<bool TransformColors>
gdImagePtr TILE_Tracker::CreateFrame_Palette_Auto(
    const VecType<uint32>& screen,
    unsigned frameno, unsigned wid, unsigned hei)
{
    return CreateFrame_TrueColor<TransformColors> (screen, frameno,wid,hei);
}

template<bool TransformColors, bool UseErrorDiffusion>
gdImagePtr TILE_Tracker::CreateFrame_Palette_Dither_With(
    const VecType<uint32>& screen,
    unsigned frameno, unsigned wid, unsigned hei,
    const Palette& pal)
{
    const unsigned max_pattern_value = DitherMatrixWidth * DitherMatrixHeight * TemporalDitherSize;

    /* First try to create a paletted image */
    gdImagePtr im = pal.Size() <= 256
        ? gdImageCreate(         wid+pad_left+pad_right,hei+pad_top+pad_bottom)
        : gdImageCreateTrueColor(wid+pad_left+pad_right,hei+pad_top+pad_bottom);

    gdImageAlphaBlending(im, 0);
    gdImageSaveAlpha(im,     1);
    if(pal.Size() <= 256)
    {
        for(unsigned a=0; a<pal.Size(); ++a)
        {
            unsigned pix = pal.GetColor(a);
            gdImageColorAllocateAlpha(im, (pix>>16)&0xFF, (pix>>8)&0xFF, pix&0xFF, (pix>>24)&0x7F);
        }
        gdImageColorAllocateAlpha(im, 0,0,0, 127); //0xFF000000u;
    }

    const unsigned ErrorDiffusionMaxHeight = 4;
    std::vector<GammaColorVec> Errors(
        UseErrorDiffusion ? ErrorDiffusionMaxHeight*(wid+8) : 0 );

    #pragma omp parallel for schedule(static,2) if(!UseErrorDiffusion)
    for(unsigned y=0; y<hei; ++y)
    {
        transform_caches_t& transform_cache = GetTransformCache();
        dither_cache_t& dither_cache = GetDitherCache(0,1);

        for(unsigned p=y*wid, x=0; x<wid; ++x)
        {
            uint32 pix = screen[p+x];
            if(pix == DefaultPixel)
                pix = 0x7F000000u;
            if(TransformColors)
                pix = DoCachedPixelTransform(transform_cache, pix,wid,hei, frameno,x,y);

            //pix &= 0xFFFCFCFCu; // speed hack

            int r = (pix >> 16)&0xFF;
            int g = (pix >>  8)&0xFF;
            int b = (pix      )&0xFF;
            int a = (pix >> 24); if(a&0x80) a>>=1;

            ColorInfo orig_colorinfo(r,g,b,a);
            GammaColorVec& orig_color = orig_colorinfo.gammac;

            if(UseErrorDiffusion)
            {
                GammaColorVec* pos = &Errors[ ((y%ErrorDiffusionMaxHeight)*(wid+8) + (x+4)) + 0];
                orig_color += *pos;
                *pos       = GammaColorVec(0.0f);

                /*GammaColorVec clamped = orig_color;
                clamped.ClampTo0and1();
                pix = clamped.GetGammaUncorrectedRGB();*/
                orig_color.ClampTo0and1();
                pix = orig_color.GetGammaUncorrectedRGB();
            }

            // Find two closest entries from palette and use o8x8 dithering
            MixingPlan output;
            if(UseDitherCache)
            {
                dither_cache_t::iterator i = dither_cache.lower_bound(pix);
                if(i == dither_cache.end() || i->first != pix)
                {
                    ColorInfo input(pix, orig_color);
                    output = FindBestMixingPlan(input, pal);
                    dither_cache.insert(i, std::make_pair(pix, output));
                }
                else
                    output = i->second;
            }
            else
            {
                /*if(x >= 128
                && (orig_colorinfo.B >= orig_colorinfo.G
                 || orig_colorinfo.luma >= 200000)
                  )
                {
                    DitherColorListSize = 64;
                }
                else if(orig_colorinfo.B >= orig_colorinfo.R)
                {
                    DitherColorListSize = 4;
                }
                else
                {
                    DitherColorListSize = 16;
                }*/

                ColorInfo input(pix, orig_color);
                output = FindBestMixingPlan(input, pal);
            }

            unsigned pattern_value =
                DitheringMatrix
                    [ ((y%DitherMatrixHeight)*DitherMatrixWidth
                     + (x%DitherMatrixWidth)
                       )// % (DitherMatrixHeight*DitherMatrixWidth)
                    ];
        #if 0
            if(output.size() == 2)
            {
                #define IsMarioPair_Reverse(a,b) \
                    (pal.GetColor(output[0]) == (a) \
                  && pal.GetColor(output[1]) == (b))
                if(IsMarioPair_Reverse(0xF80000, 0x806000)
                || IsMarioPair_Reverse(0xF80000, 0xF88000)
                || IsMarioPair_Reverse(0xF88000, 0xF8F800)
                || IsMarioPair_Reverse(0x806000, 0x00F800)
                || IsMarioPair_Reverse(0x806000, 0x008040)
                || IsMarioPair_Reverse(0x0000F8, 0x00F8F8)
                || IsMarioPair_Reverse(0xC04020, 0x0000F8)
                || IsMarioPair_Reverse(0x806000, 0xC04020)
                || IsMarioPair_Reverse(0x000000, 0x806000)
                || IsMarioPair_Reverse(0xF80000, 0xF8C080)
                || IsMarioPair_Reverse(0x808080, 0xC000C0)
                || IsMarioPair_Reverse(0x000000, 0x808080)
                || IsMarioPair_Reverse(0x808080, 0xC0C0C0)
                || IsMarioPair_Reverse(0xF8F8F8, 0xC0C0C0)
                || IsMarioPair_Reverse(0x000000, 0xC000C0))
                {
                    // Swap the items
                    unsigned a = output[0]; output[0] = output[1]; output[1] = a;
                    std::fprintf(stderr, "Swapped mario pair %06X, %06X\n",
                        pal.GetColor(output[0]), pal.GetColor(output[1]) );
                }
                #undef IsMarioPair_Reverse
            }
            /*else
                std::fprintf(stderr, "%u colors...\n", (unsigned) output.size());*/
        #endif

            //unsigned skew = x^y^(x>>1)^(y>>1)^(x<<2)^(y<<2)^(x>>3)^(y>>3);
            unsigned skew = x-y+x/3-y/5;
            unsigned temp_pos = TemporalMatrix[ (frameno+skew) % TemporalDitherSize ];

            if(TemporalDitherSize > 1)
            {
                if(TemporalDitherMSB)
                    pattern_value = pattern_value + (DitherMatrixWidth*DitherMatrixHeight)*temp_pos;
                else
                    pattern_value = pattern_value * TemporalDitherSize + temp_pos;
            }
            if(pattern_value >= max_pattern_value)
                fprintf(stderr, "ERROR: pattern_value=%u, max_pattern_value=%u\n", pattern_value, max_pattern_value);

            int color = output[ pattern_value * output.size() / max_pattern_value ];
            if(pix & 0xFF000000u) gdImageColorTransparent(im, color);
            if(pal.Size() <= 256)
                gdImageSetPixel(im, x+pad_left,y+pad_top, color);
            else
                gdImageSetPixel(im, x+pad_left,y+pad_top, pal.GetColor( color ));

            if(UseErrorDiffusion)
            {
                GammaColorVec flterror = pal.Data[color].gammac - orig_color;
                #define put(xo,yo, factor) \
                    Errors[ (((y+yo)%ErrorDiffusionMaxHeight)*(wid+8) \
                            + (x+xo+4)) ] -= flterror * (factor##f)
                switch(Diffusion)
                {
                    case Diffusion_None: break;
                    case Diffusion_FloydSteinberg:
                        put( 1,0, 7/16.0);
                        put(-1,1, 3/16.0);
                        put( 0,1, 5/16.0);
                        put( 1,1, 1/16.0);
                        break;
                    case Diffusion_JarvisJudiceNinke:
                        put( 1,0, 7/48.0);
                        put( 2,0, 5/48.0);
                        put(-2,1, 3/48.0);
                        put(-1,1, 5/48.0);
                        put( 0,1, 7/48.0);
                        put( 1,1, 5/48.0);
                        put( 2,1, 3/48.0);
                        put(-2,2, 1/48.0);
                        put(-1,2, 3/48.0);
                        put( 0,2, 5/48.0);
                        put( 1,2, 3/48.0);
                        put( 2,2, 1/48.0);
                        break;
                    case Diffusion_Stucki:
                        put( 1,0, 8/42.0);
                        put( 2,0, 4/42.0);
                        put(-2,1, 2/42.0);
                        put(-1,1, 4/42.0);
                        put( 0,1, 8/42.0);
                        put( 1,1, 4/42.0);
                        put( 2,1, 2/42.0);
                        put(-2,2, 1/42.0);
                        put(-1,2, 2/42.0);
                        put( 0,2, 4/42.0);
                        put( 1,2, 2/42.0);
                        put( 2,2, 1/42.0);
                        break;
                    case Diffusion_Burkes:
                        put( 1,0, 8/32.0);
                        put( 2,0, 4/32.0);
                        put(-2,1, 2/32.0);
                        put(-1,1, 4/32.0);
                        put( 0,1, 8/32.0);
                        put( 1,1, 4/32.0);
                        put( 2,1, 2/32.0);
                        break;
                    case Diffusion_Sierra3:
                        put( 1,0, 5/32.0);
                        put( 2,0, 3/32.0);
                        put(-2,1, 2/32.0);
                        put(-1,1, 4/32.0);
                        put( 0,1, 5/32.0);
                        put( 1,1, 4/32.0);
                        put( 2,1, 2/32.0);
                        put(-1,2, 2/32.0);
                        put( 0,2, 3/32.0);
                        put( 1,2, 2/32.0);
                        break;
                    case Diffusion_Sierra2:
                        put( 1,0, 4/16.0);
                        put( 2,0, 3/16.0);
                        put(-2,1, 1/16.0);
                        put(-1,1, 2/16.0);
                        put( 0,1, 3/16.0);
                        put( 1,1, 2/16.0);
                        put( 2,1, 1/16.0);
                        break;
                    case Diffusion_Sierra24A:
                        put( 1,0, 2/4.0);
                        put(-1,1, 1/4.0);
                        put( 0,1, 1/4.0);
                        break;
                    case Diffusion_StevensonArce:
                        put( 2,0, 32/200.0);
                        put(-3,1, 12/200.0);
                        put(-1,1, 26/200.0);
                        put( 1,1, 30/200.0);
                        put( 3,1, 16/200.0);
                        put(-2,2, 12/200.0);
                        put( 0,2, 26/200.0);
                        put( 2,2, 12/200.0);
                        put(-3,3,  5/200.0);
                        put(-1,3, 12/200.0);
                        put( 1,3, 12/200.0);
                        put( 3,3,  5/200.0);
                        break;
                    case Diffusion_Atkinson:
                        put( 1,0,  1/8.0);
                        put( 2,0,  1/8.0);
                        put(-1,1,  1/8.0);
                        put( 0,1,  1/8.0);
                        put( 1,1,  1/8.0);
                        put( 0,2,  1/8.0);
                        break;
                }
            }
        }
    }

    if(CGA16mode)
    {
        static const struct cga16_palette_initializer
        {
            unsigned colors[16*5];
            cga16_palette_initializer()
            {
                double hue = (35.0 + 0.0)*0.017453239;
                double sinhue = std::sin(hue), coshue = std::cos(hue);
                for(unsigned i=0; i<16; ++i)
                    for(unsigned j=0; j<5; ++j)
                    {
                        unsigned colorBit4 = (i&1)>>0;
                        unsigned colorBit3 = (i&2)>>1;
                        unsigned colorBit2 = (i&4)>>2;
                        unsigned colorBit1 = (i&8)>>3;
                        //calculate lookup table   
                        double I = 0, Q = 0, Y;
                        I += (double) colorBit1;
                        Q += (double) colorBit2;
                        I -= (double) colorBit3;
                        Q -= (double) colorBit4;
                        Y  = (double) j / 4.0; //calculated avarage is over 4 bits

                        double pixelI = I * 1.0 / 3.0; //I* tvSaturation / 3.0
                        double pixelQ = Q * 1.0 / 3.0; //Q* tvSaturation / 3.0
                        I = pixelI*coshue + pixelQ*sinhue;
                        Q = pixelQ*coshue - pixelI*sinhue;

                        double R = Y + 0.956*I + 0.621*Q; if (R < 0.0) R = 0.0; if (R > 1.0) R = 1.0;
                        double G = Y - 0.272*I - 0.647*Q; if (G < 0.0) G = 0.0; if (G > 1.0) G = 1.0;
                        double B = Y - 1.105*I + 1.702*Q; if (B < 0.0) B = 0.0; if (B > 1.0) B = 1.0;
                        unsigned char rr = R*0xFF, gg = G*0xFF, bb = B*0xFF;
                        colors[(j<<4)|i] = (rr << 16) | (gg << 8) | bb;
                    }
            }
        } cga16_palette;

        // FIXME: Padding is not implemented or handled properly here.
        gdImagePtr im2 = gdImageCreateTrueColor(wid*4, hei);
        std::vector<unsigned char> cga16temp(hei*(wid*4+3), 0);
        #pragma omp parallel for schedule(static,2)
        for(unsigned y=0; y<hei; ++y)
        {
            // Update colors 10..14 to 11..15 because pattern 1010 was skipped over in the palette
            for(unsigned x=0; x<wid; ++x)
            {
                unsigned i = gdImageGetPixel(im, x,y);
                if(i >= 10) gdImageSetPixel(im, x,y, i+1);
            }
            unsigned char* temp = &cga16temp[y*(wid*4+3)];
            for(unsigned x=0; x<wid*4; ++x)
                temp[x+2] = (( gdImageGetPixel(im,x>>2,y) >> (3-(x&3)) ) & 1) << 4;
            for(unsigned i=0, x=0; x<wid; ++x)
            {
                unsigned v = gdImageGetPixel(im,x,y);
                for(unsigned c=0; c<4; ++c)
                {
                    unsigned p = v | (temp[i] + temp[i+1] + temp[i+2] + temp[i+3]);
                    gdImageSetPixel(im2, i++, y,  cga16_palette.colors[p]);
                }
            }
        }
        gdImageDestroy(im);
        return im2;
    }

    return im;
}

template<bool TransformColors, bool UseErrorDiffusion>
gdImagePtr TILE_Tracker::CreateFrame_Palette_Dither(
    const VecType<uint32>& screen,
    unsigned frameno, unsigned wid, unsigned hei)
{
    return
        CreateFrame_Palette_Dither_With<TransformColors,UseErrorDiffusion>
        (screen, frameno, wid, hei, CurrentPalette);
}


template<bool TransformColors, bool UseErrorDiffusion>
gdImagePtr TILE_Tracker::CreateFrame_Palette_Dither_Sections(
    const VecType<uint32>& screen,
    unsigned frameno, unsigned wid, unsigned hei)
{
    // Verify that the dithering sections line up
   {unsigned prev = 0;
    bool error = false;
    for(auto d: DitheringSections)
    {
        if(!d.width && prev) { error=true; break; }
        if(d.width && prev && (prev % d.width)) { error=true; break; }
        if(d.combination_limit)
        {
            // TODO: support this
            std::fprintf(stderr, "Sorry, unsupported: Limited number of subpalettes per screen\n");
        }
        prev = d.width;
    }
    prev=0;
    for(auto d: DitheringSections)
    {
        if(!d.height && prev) { error=true; break; }
        if(d.height && prev && (prev % d.height)) { error=true; break; }
        prev = d.height;
    }
    if(error)
    {
        std::fprintf(stderr, "ERROR: Dithering sections do not line up. Each section should be a subsection of the previous one!\n");
        return CreateFrame_Palette_Dither<TransformColors,UseErrorDiffusion> (screen,frameno,wid,hei);
    }}

    // Create the target image.
    gdImagePtr im = gdImageCreate(wid + pad_left+pad_right, hei + pad_top+pad_bottom);
    gdImageAlphaBlending(im, false);
    gdImageSaveAlpha(im, true);
    for(size_t a=0; a<CurrentPalette.Size(); ++a)
    {
        unsigned pix = CurrentPalette.GetColor(a);
        gdImageColorAllocateAlpha(im, (pix>>16)&0xFF, (pix>>8)&0xFF, pix&0xFF, (pix>>24)&0x7F);
    }
    gdImageColorAllocateAlpha(im, 0,0,0, 127); //0xFF000000u;

    unsigned num_colors_total = 0;
    for(auto d: DitheringSections) num_colors_total += d.n_colors;

    const unsigned max_combinations_for_cache = 65536;
    std::vector<std::vector<Palette> > palette_cache(num_colors_total);

    UseDitherCache = true;
    unsigned num_combinations = 1;
    for(unsigned n=0; n<num_colors_total; ++n)
    {
        unsigned palette_size = CurrentPalette.Size();
        num_combinations *= palette_size;
        palette_cache[n].resize(num_combinations);
        if(num_combinations > max_combinations_for_cache)
            { UseDitherCache = false; break; }
    }
    for(unsigned n=0; n<num_colors_total; ++n)
    {
        unsigned palette_size = CurrentPalette.Size();
        std::vector<Palette>& p = palette_cache[n];
        std::vector<unsigned> counter(n+1);
        for(size_t c=0; c<p.size(); ++c)
        {
            //fprintf(stderr, "%u-color palette %zu/%zu: ", n+1, c, p.size());
            for(int i=n; i>=0; --i)
            {
                //fprintf(stderr, " %u", counter[i]);
                p[c].AddColorFrom(CurrentPalette, counter[i]);
            }
            //fprintf(stderr, "\n");
            p[c].Analyze();
            for(int i=n; i>=0; counter[i--] = 0)
                if(++counter[i] < palette_size)
                    break;
        }
    }

    std::function<void(size_t,unsigned,unsigned,unsigned,unsigned,const std::vector<unsigned>&)>
        DoSection = [&]
        (size_t section_index, unsigned x0,unsigned y0, unsigned x1,unsigned y1,
         const std::vector<unsigned>& in_colors)
    {
        unsigned palette_size = CurrentPalette.Size();

        if(section_index == DitheringSections.size())
        {
            // Render this section using the supplied colors.
            fprintf(stderr, "Chose for (%u,%u)-(%u,%u):", x0,y0, x1,y1);
            for(auto ic: in_colors) fprintf(stderr, " %u", ic);
            fprintf(stderr, "\n");

            unsigned pal_index = 0;
            for(size_t p=1,i2=0; i2<in_colors.size(); ++i2, p*=palette_size)
                pal_index += p * in_colors[i2];
            dither_cache_t& dither_cache2 =
                GetDitherCache(UseDitherCache?pal_index:0,
                               UseDitherCache?num_combinations:1);

            // Got all colors we want. Render this slot.
            Palette wip_palette;
            bool usewip = palette_cache[ in_colors.size()-1 ].empty();
            Palette& pal =
                usewip ? wip_palette : palette_cache[ in_colors.size()-1 ][pal_index ];
            if(usewip)
            {
                for(auto ic: in_colors) pal.AddColorFrom( CurrentPalette, ic );
                pal.Analyze();
            }

            transform_caches_t& transform_cache = GetTransformCache();
            int pl=pad_left,pr=pad_right, pt=pad_top, pb=pad_bottom;
            for(unsigned y=y0; y<y1; y+=1)
                for(unsigned x=x0; x<x1; x+=1)
                {
                    int color = GetMixColor<TransformColors>
                        (dither_cache2,
                         transform_cache, wid,hei,frameno,x,y, screen[y*wid+x], pal);
                    color = in_colors[color];
                    if(x == 0)
                    {
                        if(y == 0)     gdImageFilledRectangle(im, 0,0,      wid+pl+pr,        pt-1, in_colors[0]);
                        gdImageLine(im, 0,y+pt, x+pl-1,y+pt, in_colors[0]);
                    }
                    if(x == wid-1)
                    {
                        if(y == hei-1) gdImageFilledRectangle(im, 0,y+pt+1, wid+pl+pr, hei+pt+pb-1, in_colors[0]);
                        gdImageLine(im, x+pl+1, y+pt, wid+pl+pr,y+pt, in_colors[0]);
                    }
                    gdImageSetPixel(im, x+pl, y+pt, color);
                }
            return;
        }

        // Still need to accumulate more colors.
        const auto& d = DitheringSections[section_index];
        // Number of colors to choose for each slot of this section
        unsigned colors = d.n_colors;
        // Determine the geometry of slots in this section
        unsigned width  = d.width;  unsigned w = width  ? width  : wid;
        unsigned height = d.height; unsigned h = height ? height : hei;

      #if 0
        if(colors == 1)
        {
            // Dither the image using the entire palette and choose the most common color.
            transform_caches_t& transform_cache = GetTransformCache();

            dither_cache_t& dither_cache2 = GetDitherCache(0,1);

            std::map<int, unsigned, std::less<int>, FSBAllocator<int> > tally;
            for(unsigned y=y0; y<y1; ++y)
                for(unsigned x=x0; x<x1; ++x)
                {
                    int color = GetMixColor<TransformColors>
                        (dither_cache2,transform_cache, wid,hei,frameno,x,y, screen[y*wid+x], CurrentPalette);
                    tally[color] += 1;
                }
            std::vector< std::pair<unsigned,int> > sortvec;
            sortvec.reserve( tally.size() );
            for(auto i: tally) sortvec.push_back( { i.second, i.first } );
            std::sort(sortvec.begin(), sortvec.end());
            unsigned chosen_color = sortvec.back().second;
            std::vector<unsigned> chosen( in_colors );
            chosen.push_back(chosen_color);
            // Got colors. Accumulate more colors, or output the image.
            DoSection( section_index+1, x0,y0, x1,y1, chosen);
            return;
        }
      #endif

        // How many colors do we have as _choices_?
        const unsigned max_pattern_value = DitherMatrixWidth * DitherMatrixHeight;

        // Process each slot.
        #pragma omp parallel for schedule(static) collapse(2)
        for(unsigned by = y0; by < y1; by += h)
            for(unsigned bx = x0; bx < x1; bx += w)
            {
                transform_caches_t& transform_cache = GetTransformCache();
                unsigned ey = std::min(hei, by+h);
                unsigned ex = std::min(wid, bx+w);
                /*fprintf(stderr, "Round %zu/%zu: section (%u,%u)-(%u,%u) of (%u,%u)-(%u,%u) of %u,%u\n",
                    section_index, DitheringSections.size(),
                    bx,by, ex,ey, x0,y0, x1,y1, wid,hei);*/

                // Generate colors for this slot.
                std::vector<unsigned> chosen( in_colors );
                chosen.resize( in_colors.size() + colors );

                double bestdiff = -1;
                bool refined = true;

                unsigned try_space = 1;
                for(unsigned i=0; i<in_colors.size()+colors; ++i) try_space *= palette_size;
                std::vector<bool> used_tries( try_space, false );

                for(unsigned tries=0; refined; ++tries)
                {
                    refined = false;
                    // Each color:
                    for(unsigned i=0; i<colors; ++i)
                    {
                        Palette wip_palette;
                        for(auto ic: chosen) wip_palette.AddColorFrom( CurrentPalette, ic );
                        //fprintf(stderr, "chosen size=%zu, palette_cache size %zu\n", chosen.size(), palette_cache.size());
                        auto& cache_palettes = palette_cache.at(chosen.size()-1);
                        bool usewip = cache_palettes.empty();

                        for(unsigned c=0; c<palette_size; ++c)
                        {
                            // Don't create a duplicate to an already chosen color
                            bool match=false;
                            for(auto ic: chosen) if(c == ic) { match=true; break; }
                            if(match) continue;

                            unsigned pal_index = 0;
                            for(size_t p=1,i2=0; i2<chosen.size(); ++i2, p*=palette_size)
                                pal_index += p * (i2==(in_colors.size()+i) ? c : chosen[i2]);

                            if(used_tries.at(pal_index)) continue;
                            used_tries[pal_index] = true;

                            dither_cache_t& dither_cache2 =
                                GetDitherCache(UseDitherCache?pal_index:0,
                                               UseDitherCache?num_combinations:1);

                            // Profile this palette.
                            Palette& pal =
                                usewip ? wip_palette
                                       : cache_palettes.at(pal_index);
                            if(usewip)
                            {
                                pal.ReplaceColorFrom( in_colors.size() + i, CurrentPalette, c );
                                pal.Analyze();
                            }
                            Averaging av;
                            av.Reset();
                            for(unsigned y=by; y<ey; y+=1)
                                //for(unsigned x=bx + ((~y % 5)^2); x<ex; x += 5)
                                for(unsigned x=bx; x<ex; x += 1)
                                {
                                    uint32 pix1 = screen[y*wid+x];
                                    if(pix1 == DefaultPixel) pix1 = 0x7F000000u;
                                    if(TransformColors) pix1 = DoCachedPixelTransform(transform_cache, pix1,wid,hei, frameno,x,y);
                                    ColorInfo input(pix1);

                                    // Create a mixing plan
                                    MixingPlan output;
                                    if(UseDitherCache)
                                    {
                                        dither_cache_t::iterator i = dither_cache2.lower_bound(pix1);
                                        if(i == dither_cache2.end() || i->first != pix1)
                                        {
                                            output = FindBestMixingPlan(input, pal);
                                            dither_cache2.insert(i, std::make_pair(pix1, output));
                                        }
                                        else
                                            output = i->second;
                                    }
                                    else
                                    {
                                        output = FindBestMixingPlan(input, pal);
                                    }
                                #if 1
                                    // Mix the colors together and compare the result to original.
                                    GammaColorVec our_sum(0.0);
                                    for(auto a: output) our_sum += pal.GetMeta(a).gammac;
                                    GammaColorVec combined = our_sum * (1 / double(output.size()));
                                    av.Cumulate( ColorCompare( input, ColorInfo(combined) ) , 12 );
                                #endif
                                #if 1
                                    // To prevent the ditherer being _too_ optimistic,
                                    // also add the raw dithered pixel to the equation
                                    unsigned pattern_value =
                                        DitheringMatrix
                                            [ ((y%DitherMatrixHeight)*DitherMatrixWidth
                                             + (x%DitherMatrixWidth)
                                               )// % (DitherMatrixHeight*DitherMatrixWidth)
                                            ];
                                    int color = output[ pattern_value * output.size() / max_pattern_value ];
                                    av.Cumulate( ColorCompare( input, pal.GetMeta(color) ) );
                                #endif
                                }
                            double diff = av.GetValue();

                            /*fprintf(stderr, "Trying %zu-color palette %u:", chosen.size(), pal_index);
                            for(size_t i2=0; i2<chosen.size(); ++i2)
                                fprintf(stderr, " %u", (i2==(in_colors.size()+i) ? c : chosen[i2]) );
                            fprintf(stderr, " (wip=%s) -gets %g\n", usewip?"true":"false", diff);*/

                            if(diff < 0)
                                fprintf(stderr, "ERROR: diff = %g\n", diff);
                            if(diff < bestdiff || bestdiff < 0)
                                { bestdiff = diff; chosen[in_colors.size() + i] = c;
                                  //fprintf(stderr, "Chose [%u] = %u (round %zu, %u colors)\n", i,c, section_index,colors);
                                  refined = true; }
                        }
                    }
                }

                // Got colors. Accumulate more colors, or output the image.
                DoSection( section_index+1, bx,by, ex,ey, chosen );
            }
    };

    // For each DitheringSection, start accumulating colors for the slots.
    DoSection(0, 0,0, wid,hei, std::vector<unsigned>());
    return im;
}

AlignResult TILE_Tracker::TryAlignWithHotspots
    (const uint32* input, unsigned sx,unsigned sy) const
{
    /* Find spots of interest within the reference image,
     * and within the input image.
     *
     * Select that offset which results in greatest overlap
     * between those two sets of spots.
     */

    std::vector<InterestingSpot> input_spots;
    std::vector<InterestingSpot> reference_spots;
    FindInterestingSpots(input_spots, input, 0,0, sx,sy, true);

    /* Cache InterestingSpot lists for each cube */
    static std::map
        <IntCoordinate, std::vector<InterestingSpot>,
         std::less<IntCoordinate>,
         FSBAllocator<int> > cache;

    /* For speed reasons, we don't use LoadScreen(), but
     * instead, work on cube-by-cube basis.
     */
    for(ymaptype::const_iterator
        yi = screens.begin();
        yi != screens.end();
        ++yi)
    {
        const int y_screen_offset = yi->first * 256;

        for(xmaptype::const_iterator
            xi = yi->second.begin();
            xi != yi->second.end();
            ++xi)
        {
            const int x_screen_offset = xi->first  * 256;
            const cubetype& cube      = xi->second;

            IntCoordinate cache_key = {x_screen_offset,y_screen_offset};

            if(cube.changed)
            {
                uint32 result[256*256];

                cube.pixels->GetStaticInto(result, 256);

                size_t prev_size = reference_spots.size();
                FindInterestingSpots(reference_spots, result,
                    x_screen_offset,y_screen_offset,
                    256,256,
                    false);

                cache[cache_key].assign(
                    reference_spots.begin() + prev_size,
                    reference_spots.end() );

                cube.changed = false;
            }
            else
            {
                const std::vector<InterestingSpot>& found = cache[cache_key];
                reference_spots.insert(
                    reference_spots.end(),
                    found.begin(),
                    found.end());
            }
        }
    }

    return Align(
        input_spots,
        reference_spots,
        org_x, org_y);
}

AlignResult TILE_Tracker::TryAlignWithBackground
    (const uint32* input, unsigned sx,unsigned sy) const
{
    struct AlignResult align =
        Align(
            &LoadBackground(xmin,ymin, xmax-xmin,ymax-ymin)[0],
            xmax-xmin, ymax-ymin,
            input,
            sx, sy,
            org_x-xmin,
            org_y-ymin
        );

    align.offs_x -= org_x-xmin;
    align.offs_y -= org_y-ymin;
    return align;
}

AlignResult TILE_Tracker::TryAlignWithPrevFrame
    (const uint32* prev_input,
     const uint32* input, unsigned sx,unsigned sy) const
{
    return Align(
        prev_input, sx,sy,
        input,      sx,sy,
        0,0
    );
}

void
TILE_Tracker::FitScreenAutomatic
    (const uint32* input, unsigned sx,unsigned sy)
{
    static VecType<uint32> prev_frame;
    //fprintf(stderr, "sx=%u,sy=%u, prev_frame size=%u\n", sx,sy, prev_frame.size());
    if(prev_frame.size() == sx*sy)
    {
        AlignResult align = TryAlignWithPrevFrame(&prev_frame[0], input,sx,sy);
        if(!align.suspect_reset)
        {
            prev_frame.assign(input, input+sx*sy);
            FitScreen(input,sx,sy, align);
            return;
        }
    }
    prev_frame.assign(input, input+sx*sy);

    AlignResult align = TryAlignWithHotspots(input,sx,sy);
    FitScreen(input,sx,sy, align);
}

void TILE_Tracker::FitScreen
    (const uint32* input, unsigned sx, unsigned sy,
     const AlignResult& alignment,
     int extra_offs_x,
     int extra_offs_y
    )
{
    //if(alignment.offs_x != 0 || alignment.offs_y != 0)
    {
        std::fprintf(stderr, "[frame%5u] Motion(%d,%d), Origo(%d,%d)\n",
            CurrentTimer, alignment.offs_x,alignment.offs_y, org_x,org_y);
    }

    org_x += alignment.offs_x; org_y += alignment.offs_y;

    int this_org_x = org_x + extra_offs_x;
    int this_org_y = org_y + extra_offs_y;

    if(alignment.suspect_reset)
    {
#if 0
        goto AlwaysReset;
#endif
        VecType<uint32> oldbuf = LoadScreen(this_org_x,this_org_y, sx,sy, CurrentTimer, bgmethod);
        unsigned diff = 0;
        for(unsigned a=0; a<oldbuf.size(); ++a)
        {
            unsigned oldpix = oldbuf[a];
            unsigned pix   = input[a];
            unsigned r = (pix >> 16) & 0xFF;
            unsigned g = (pix >> 8) & 0xFF;
            unsigned b = (pix    ) & 0xFF;
            unsigned oldr = (oldpix >> 16) & 0xFF;
            unsigned oldg = (oldpix >> 8) & 0xFF;
            unsigned oldb = (oldpix    ) & 0xFF;
            int rdiff = (int)(r-oldr); if(rdiff < 0)rdiff=-rdiff;
            int gdiff = (int)(g-oldg); if(gdiff < 0)gdiff=-gdiff;
            int bdiff = (int)(b-oldb); if(bdiff < 0)bdiff=-bdiff;
            unsigned absdiff = rdiff+gdiff+bdiff;
            diff += absdiff;
        }

        if(diff > oldbuf.size() * 128)
        {
#if 0
            /* Castlevania hack */
            static int org_diff = -180;
            org_y += org_diff;
            org_diff = -org_diff;
#else
#if 1
        //AlwaysReset:
            Save();
            Reset();
#endif
#endif
        }
    }

    const bool first = CurrentTimer == 0;
    if(first || this_org_x < xmin) xmin = this_org_x;
    if(first || this_org_y < ymin) ymin = this_org_y;
    int xtmp = this_org_x+sx; if(first || xtmp > xmax) xmax=xtmp;
    int ytmp = this_org_y+sy; if(first || ytmp > ymax) ymax=ytmp;

#if 0
    /* If the image geometry would exceed some bounds */
    if(xmax-xmin > 800 || ymax-ymin > 800)
    {
        SaveAndReset();
        first=true;
    }
#endif

    PutScreen(input, this_org_x,this_org_y, sx,sy, CurrentTimer);
}

void TILE_Tracker::Reset()
{
    SequenceBegin += CurrentTimer;
    CurrentTimer = 0;

    std::fprintf(stderr, " Resetting\n");
    screens.clear();
    org_x = 0x40000000;
    org_y = 0x40000000;
    xmin=xmax=org_x;
    ymin=ymax=org_y;
}

void TILE_Tracker::NextFrame()
{
    std::printf("/*%5u*/ %d,%d,\n",
        CurrentTimer,
        org_x - xmin,
        org_y - ymin
        );
    std::fflush(stdout);
    ++CurrentTimer;
}
