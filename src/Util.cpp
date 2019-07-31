#include "Util.h"

#include <cmath>
#include <complex>

#include "Biquad.h"

namespace GstDsp
{

template <typename T>
bool computeBiQuad(int r, const Filter& f, T* biquad)
{
    switch (f.type) {
    case FilterType::Peak: {
        double A = pow(10, f.g/40.0);
        double w0 = 2*M_PI*f.f/r;
        double alpha = sin(w0)*0.5/f.q;

        double alpha1 = alpha*A;
        double alpha2 = alpha/A;
        double a0     = 1.0 + alpha2;

        biquad->b0 = ( 1.0 + alpha1 ) / a0;
        biquad->b1 = (-2.0 * cos(w0)) / a0;
        biquad->b2 = ( 1.0 - alpha1 ) / a0;
        biquad->a1 = biquad->b1;
        biquad->a2 = ( 1.0 - alpha2 ) / a0;
        break;
    }
    case FilterType::LowPass: {
        double w0 = 2*M_PI*f.f/r;
        double alpha = sin(w0)*0.5/f.q;
        double a0    = 1.0 + alpha;

        biquad->b1 = ( 1.0 - cos(w0) ) / a0;
        biquad->b0 = biquad->b1 * 0.5;
        biquad->b2 = biquad->b0;
        biquad->a1 = (-2.0 * cos(w0)) / a0;
        biquad->a2 = ( 1.0 - alpha  ) / a0;
        break;
    }
    case FilterType::HighPass: {
        double w0 = 2*M_PI*f.f/r;
        double alpha = sin(w0)*0.5/f.q;
        double a0    = 1.0 + alpha;

        biquad->b1 = -( 1.0 + cos(w0) ) / a0;
        biquad->b0 = biquad->b1 * -0.5;
        biquad->b2 = biquad->b0;
        biquad->a1 = (-2.0 * cos(w0)) / a0;
        biquad->a2 = ( 1.0 - alpha  ) / a0;
        break;
    }
    case FilterType::LowShelf: {
        double A = pow(10, f.g/40.0);
        double w0 = 2*M_PI*f.f/r;
        double cosW0 = cos(w0);
        double alpha = sin(w0)*0.5/f.q;
        double sqrtAalpha2 = 2.0*sqrt(A)*alpha;
        double a0 = (A+1) + (A-1)*cosW0 + sqrtAalpha2;

        biquad->b0 =    A*( (A+1) - (A-1)*cosW0 + sqrtAalpha2) / a0;
        biquad->b1 =  2*A*( (A-1) - (A+1)*cosW0              ) / a0;
        biquad->b2 =    A*( (A+1) - (A-1)*cosW0 - sqrtAalpha2) / a0;
        biquad->a1 =   -2*( (A-1) + (A+1)*cosW0              ) / a0;
        biquad->a2 =      ( (A+1) + (A-1)*cosW0 - sqrtAalpha2) / a0;
        break;
    }
    case FilterType::HighShelf: {
        double A = pow(10, f.g/40.0);
        double w0 = 2*M_PI*f.f/r;
        double cosW0 = cos(w0);
        double alpha = sin(w0)*0.5/f.q;
        double sqrtAalpha2 = 2.0*sqrt(A)*alpha;
        double a0 = (A+1) - (A-1)*cosW0 + sqrtAalpha2;

        biquad->b0 =    A*( (A+1) + (A-1)*cosW0 + sqrtAalpha2) / a0;
        biquad->b1 = -2*A*( (A-1) + (A+1)*cosW0              ) / a0;
        biquad->b2 =    A*( (A+1) + (A-1)*cosW0 - sqrtAalpha2) / a0;
        biquad->a1 =    2*( (A-1) - (A+1)*cosW0              ) / a0;
        biquad->a2 =      ( (A+1) - (A-1)*cosW0 - sqrtAalpha2) / a0;
        break;
    }
    case FilterType::Invalid:
        return false;
    }

    return true;
}
template bool computeBiQuad<BiquadCoeffs>(int sampleRate, const Filter& filter, BiquadCoeffs* biquad);

bool computeResponse(const Filter& f, const std::vector<float>& freqs, std::vector<float>* mags, std::vector<float>* phases)
{
    BiquadCoeffs biquad;
    if (!computeBiQuad(48000, f, &biquad)) return false;

    mags->resize(freqs.size());
    if (phases) phases->resize(freqs.size());

    for (size_t i = 0; i < freqs.size(); ++i) {
        double w = 2.0*M_PI*freqs.at(i)/48000;
        std::complex<double> z(cos(w), sin(w));
        std::complex<double> numerator = biquad.b0 + (biquad.b1 + biquad.b2*z)*z;
        std::complex<double> denominator = 1.0 + (biquad.a1 + biquad.a2*z)*z;
        std::complex<double> response = numerator / denominator;
        mags->at(i)     = 20.0*log10(abs(response));
        if (phases) phases->at(i)   = (180.0/M_PI)*atan2(response.imag(), response.real());
    }

    return true;
}

std::ostream& operator<<(std::ostream& os, const Gst::PadDirection& direction)
{
    switch (direction) {
    case Gst::PAD_SRC:
        os << "Out";
        break;
    case Gst::PAD_SINK:
        os << "In";
        break;
    default:
        os << "Invalid";
        break;
    }

    return os;
}

} // namespace GstDsp
