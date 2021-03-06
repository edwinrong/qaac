#ifndef SOXCMODULE_H
#define SOXCMODULE_H

#include "libsoxconvolver.h"
#include "dl.h"

class SoXConvolverModule {
    DL m_dl;
public:
    SoXConvolverModule() {}
    explicit SoXConvolverModule(const std::wstring &path);
    bool loaded() const { return m_dl.loaded(); }

    const char *(*version)();
    lsx_convolver_t *(*create)(unsigned, double *, unsigned, unsigned);
    void (*close)(lsx_convolver_t *);
    void (*process)(lsx_convolver_t *, const float *, float *,
                    size_t *, size_t *);
    void (*process_ni)(lsx_convolver_t *, const float * const *, float **,
                       size_t, size_t, size_t *, size_t *);
    double *(*design_lpf)(double, double, double, double, int *, int, double);
    void (*free)(void *);
};

#endif
