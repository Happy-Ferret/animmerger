class LastPixel
{
    unsigned pix;
public:
    LastPixel() : pix(DefaultPixel)
    {
    }
    void set(unsigned R,unsigned G,unsigned B)
    {
        set(((R) << 16) + ((G) << 8) + B);
    }
    void set(uint32 p)
    {
        pix = p;
    }
    operator uint32() const { return pix; }
    void Compress()
    {
    }
};

