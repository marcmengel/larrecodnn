////////////////////////////////////////////////////////////////////////////////////////////////////
// Class:       PointIdAlg
// Author:      P.Plonski, R.Sulej (Robert.Sulej@cern.ch), D.Stefan, May 2016
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "larreco/RecoAlg/ImagePatternAlgs/PointIdAlg/DataProviderAlg.h"

#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"

#include "larcore/CoreUtils/ServiceUtil.h" // lar::providerFrom<>()

#include "larevt/CalibrationDBI/Interface/ChannelStatusService.h"
#include "larevt/CalibrationDBI/Interface/ChannelStatusProvider.h"

#include "messagefacility/MessageLogger/MessageLogger.h"

#include "CLHEP/Random/RandGauss.h"

img::DataProviderAlg::DataProviderAlg(const Config& config) :
	fCryo(9999), fTPC(9999), fPlane(9999),
	fNWires(0), fNDrifts(0), fNScaledDrifts(0), fNCachedDrifts(0),
	fDownscaleMode(img::DataProviderAlg::kMax), fDriftWindow(10),
	fCalorimetryAlg(config.CalorimetryAlg()),
	fGeometry( &*(art::ServiceHandle<geo::Geometry>()) ),
	fDetProp(lar::providerFrom<detinfo::DetectorPropertiesService>()),
	fNoiseSigma(0), fCoherentSigma(0)
{
	fCalorimetryAlg.reconfigure(config.CalorimetryAlg());
	fCalibrateAmpl = config.CalibrateAmpl();
	if (fCalibrateAmpl)
	{
	    fAmplCalibConst.resize(fGeometry->MaxPlanes());
	    mf::LogInfo("DataProviderAlg") << "Using calibration constants:";
	    for (size_t p = 0; p < fAmplCalibConst.size(); ++p)
	    {
	        try
	        {
	            fAmplCalibConst[p] = 1.2e-3 * fCalorimetryAlg.ElectronsFromADCPeak(1.0, p);
    	        mf::LogInfo("DataProviderAlg") << "   plane:" << p << " const:" << 1.0 / fAmplCalibConst[p];
    	    }
    	    catch (...) { fAmplCalibConst[p] = 1.0; }
	    }
	}

	fDriftWindow = config.DriftWindow();
	fDownscaleFullView = config.DownscaleFullView();
	fDriftWindowInv = 1.0 / fDriftWindow;

	std::string mode_str = config.DownscaleFn();
	mf::LogVerbatim("DataProviderAlg") << "Downscale mode is: " << mode_str;
	if (mode_str == "maxpool")
	{
	    //fnDownscale = [this](std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) { downscaleMax(dst, adc, tick0); };
	    fDownscaleMode = img::DataProviderAlg::kMax;
	}
	else if (mode_str == "maxmean")
	{
	    //fnDownscale = [this](std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) { downscaleMaxMean(dst, adc, tick0); };
	    fDownscaleMode = img::DataProviderAlg::kMaxMean;
	}
	else if (mode_str == "mean")
	{
	    //fnDownscale = [this](std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) { downscaleMean(dst, adc, tick0); };
	    fDownscaleMode = img::DataProviderAlg::kMean;
	}
	else
	{
		mf::LogError("DataProviderAlg") << "Downscale mode string not recognized, set to max pooling.";
		//fnDownscale = [this](std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) { downscaleMax(dst, adc, tick0); };
		fDownscaleMode = img::DataProviderAlg::kMax;
	}

    fBlurKernel = config.BlurKernel();
    fNoiseSigma = config.NoiseSigma();
    fCoherentSigma = config.CoherentSigma();
}
// ------------------------------------------------------

img::DataProviderAlg::~DataProviderAlg(void)
{
}
// ------------------------------------------------------

void img::DataProviderAlg::resizeView(size_t wires, size_t drifts)
{
    fNWires = wires; fNDrifts = drifts;
    fNScaledDrifts = drifts / fDriftWindow;

    if (fDownscaleFullView) { fNCachedDrifts = fNScaledDrifts; }
    else { fNCachedDrifts = fNDrifts; }

    fWireChannels.resize(wires);
    std::fill(fWireChannels.begin(), fWireChannels.end(), raw::InvalidChannelID);

    fWireDriftData.resize(wires);
    for (auto & w : fWireDriftData)
    {
    	w.resize(fNCachedDrifts);
    	std::fill(w.begin(), w.end(), 0.0F);
    }

    fLifetimeCorrFactors.resize(fNDrifts);
    for (size_t t = 0; t < fNDrifts; ++t)
    {
        fLifetimeCorrFactors[t] = fCalorimetryAlg.LifetimeCorrection(t);
    }
}
// ------------------------------------------------------

float img::DataProviderAlg::poolMax(int wire, int drift, size_t r) const
{
    size_t rw = r, rd = r;
    if (!fDownscaleFullView) { rd *= fDriftWindow; }

    size_t didx = getDriftIndex(drift);
    int d0 = didx - rd; if (d0 < 0) { d0 = 0; }
    int d1 = didx + rd; if (d1 >= (int)fNCachedDrifts) { d1 = fNCachedDrifts - 1; }

    int w0 = wire - rw; if (w0 < 0) { w0 = 0; }
    int w1 = wire + rw; if (w1 >= (int)fNWires) { w1 = fNWires - 1; }

    float adc, max_adc = 0;
    for (int w = w0; w <= w1; ++w)
    {
        auto const * col = fWireDriftData[w].data();
        for (int d = d0; d <= d1; ++d)
        {
            adc = col[d]; if (adc > max_adc) { max_adc = adc; }
        }
    }

    return max_adc;
}
// ------------------------------------------------------

float img::DataProviderAlg::poolSum(int wire, int drift, size_t r) const
{
    size_t rw = r, rd = r;
    if (!fDownscaleFullView) { rd *= fDriftWindow; }

    size_t didx = getDriftIndex(drift);
    int d0 = didx - rd; if (d0 < 0) { d0 = 0; }
    int d1 = didx + rd; if (d1 >= (int)fNCachedDrifts) { d1 = fNCachedDrifts - 1; }

    int w0 = wire - rw; if (w0 < 0) { w0 = 0; }
    int w1 = wire + rw; if (w1 >= (int)fNWires) { w1 = fNWires - 1; }

    float sum = 0;
    for (int w = w0; w <= w1; ++w)
    {
        auto const * col = fWireDriftData[w].data();
        for (int d = d0; d <= d1; ++d) { sum += col[d]; }
    }

    return sum;
}
// ------------------------------------------------------

void img::DataProviderAlg::downscaleMax(std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) const
{
	for (size_t i = 0; i < dst.size(); ++i)
	{
		size_t k0 = i * fDriftWindow;
		size_t k1 = (i + 1) * fDriftWindow;

		float max_adc = adc[k0] * fLifetimeCorrFactors[k0 + tick0];
		for (size_t k = k0 + 1; k < k1; ++k)
		{
			float ak = adc[k] * fLifetimeCorrFactors[k + tick0];
			if (ak > max_adc) max_adc = ak;
		}

		dst[i] = scaleAdcSample(max_adc);
	}
}

void img::DataProviderAlg::downscaleMaxMean(std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) const
{
	for (size_t i = 0; i < dst.size(); ++i)
	{
		size_t k0 = i * fDriftWindow;
		size_t k1 = (i + 1) * fDriftWindow;

		size_t max_idx = k0;
		float max_adc = adc[k0] * fLifetimeCorrFactors[k0 + tick0];
		for (size_t k = k0 + 1; k < k1; ++k)
		{
			float ak = adc[k] * fLifetimeCorrFactors[k + tick0];
			if (ak > max_adc) { max_adc = ak; max_idx = k; }
		}

		size_t n = 1;
		if (max_idx > 0) { max_adc += adc[max_idx - 1] * fLifetimeCorrFactors[max_idx - 1 + tick0]; n++; }
		if (max_idx + 1 < adc.size()) { max_adc += adc[max_idx + 1] * fLifetimeCorrFactors[max_idx + 1 + tick0]; n++; }

		dst[i] = scaleAdcSample(max_adc / n);
	}
}

void img::DataProviderAlg::downscaleMean(std::vector<float> & dst, std::vector<float> const & adc, size_t tick0) const
{
	for (size_t i = 0; i < dst.size(); ++i)
	{
		size_t k0 = i * fDriftWindow;
		size_t k1 = (i + 1) * fDriftWindow;

		float sum_adc = 0;
		for (size_t k = k0; k < k1; ++k)
		{
			sum_adc += adc[k] * fLifetimeCorrFactors[k + tick0];
		}

		if (sum_adc == 0) { dst[i] = 0; } // most cases
		else { dst[i] = scaleAdcSample(sum_adc * fDriftWindowInv); }
	}
}

bool img::DataProviderAlg::setWireData(std::vector<float> const & adc, size_t wireIdx)
{
   	if (wireIdx >= fWireDriftData.size()) return false;
   	auto & wData = fWireDriftData[wireIdx];

    if (fDownscaleFullView)
    {
        if (adc.size() / fDriftWindow <= fNCachedDrifts) { downscale(wData, adc, 0); return true; }
        else { return false; }
    }
    else
    {
        if (adc.size() <= fNCachedDrifts) // copy ADC's, no downsampling nor scaling
        {
            for (size_t i = 0; i < adc.size(); ++i) { wData[i] = adc[i]; }
        }
        else { return false; }
    }
    return true;
}
// ------------------------------------------------------

bool img::DataProviderAlg::setWireDriftData(const std::vector<recob::Wire> & wires,
	unsigned int plane, unsigned int tpc, unsigned int cryo)
{
    mf::LogInfo("DataProviderAlg") << "Create image for cryo:"
        << cryo << " tpc:" << tpc << " plane:" << plane;

	fCryo = cryo; fTPC = tpc; fPlane = plane;

	size_t nwires = fGeometry->Nwires(plane, tpc, cryo);
	size_t ndrifts = fDetProp->NumberTimeSamples();

	resizeView(nwires, ndrifts);

    bool allWrong = true;
    for (auto const & wire : wires)
	{
		auto wireChannelNumber = wire.Channel(); // ************* remember to add check for good channels in real data

		size_t w_idx = 0;
		for (auto const& id : fGeometry->ChannelToWire(wireChannelNumber))
		{
			if ((id.Plane == plane) && (id.TPC == tpc) && (id.Cryostat == cryo))
			{
			    w_idx = id.Wire;

			    auto adc = wire.Signal();
			    if (adc.size() < ndrifts)
			    {
			    	mf::LogWarning("DataProviderAlg") << "Wire ADC vector size lower than NumberTimeSamples.";
			    	continue; // not critical, maybe other wires are OK, so continue
			    }

			    if (!setWireData(adc, w_idx))
			    {
			    	mf::LogWarning("DataProviderAlg") << "Wire data not set.";
			    	continue; // also not critical, try to set other wires
			    }

			    fWireChannels[w_idx] = wireChannelNumber;
			    allWrong = false;
			}
		}
	}
	if (allWrong)
	{
	    mf::LogError("DataProviderAlg") << "Wires data not set in the cryo:"
	        << cryo << " tpc:" << tpc << " plane:" << plane;
	    return false;
	}
	
    applyBlur();
    addWhiteNoise();
    addCoherentNoise();
	
	return true;
}
// ------------------------------------------------------

float img::DataProviderAlg::scaleAdcSample(float val) const
{
    if (val < -50.) val = -50.;
    if (val > 150.) val = 150.;

    if (fCalibrateAmpl) { val *= fAmplCalibConst[fPlane]; }

    return 0.1 * val;
}
// ------------------------------------------------------

void img::DataProviderAlg::applyBlur()
{
    if (fBlurKernel.size() < 2) return;

    size_t margin_left = (fBlurKernel.size()-1) >> 1, margin_right = fBlurKernel.size() - margin_left - 1;

    std::vector< std::vector<float> > src(fWireDriftData.size());
    for (size_t w = 0; w < fWireDriftData.size(); ++w) { src[w] = fWireDriftData[w]; }

    for (size_t w = margin_left; w < fWireDriftData.size() - margin_right; ++w)
    {
        for (size_t d = 0; d < fWireDriftData[w].size(); ++d)
        {
            float sum = 0;
            for (size_t i = 0; i < fBlurKernel.size(); ++i)
            {
                sum += fBlurKernel[i] * src[w + i - margin_left][d];
            }
            fWireDriftData[w][d] = sum;
        }
    }
}
// ------------------------------------------------------

void img::DataProviderAlg::addWhiteNoise()
{
    if (fNoiseSigma == 0) return;

    double effectiveSigma = scaleAdcSample(fNoiseSigma);
    if (fDownscaleFullView) effectiveSigma /= fDriftWindow;

    CLHEP::RandGauss gauss(fRndEngine);
    std::vector<double> noise(fNCachedDrifts);
    for (auto & wire : fWireDriftData)
    {
        gauss.fireArray(fNCachedDrifts, noise.data(), 0., effectiveSigma);
        for (size_t d = 0; d < wire.size(); ++d)
        {
            wire[d] += noise[d];
        }
    }
}
// ------------------------------------------------------

void img::DataProviderAlg::addCoherentNoise()
{
    if (fCoherentSigma == 0) return;

    double effectiveSigma = scaleAdcSample(fCoherentSigma);
    if (fDownscaleFullView) effectiveSigma /= fDriftWindow;

    CLHEP::RandGauss gauss(fRndEngine);
    std::vector<double> amps1(fWireDriftData.size());
    std::vector<double> amps2(1 + (fWireDriftData.size() / 32));
    gauss.fireArray(amps1.size(), amps1.data(), 1., 0.1); // 10% wire-wire ampl. variation
    gauss.fireArray(amps2.size(), amps2.data(), 1., 0.1); // 10% group-group ampl. variation

    double group_amp = 1.0;
    std::vector<double> noise(fNCachedDrifts);
    for (size_t w = 0; w < fWireDriftData.size(); ++w)
    {
        if ((w & 31) == 0)
        {
            group_amp = amps2[w >> 5]; // div by 32
            gauss.fireArray(fNCachedDrifts, noise.data(), 0., effectiveSigma);
        } // every 32 wires

        auto & wire = fWireDriftData[w];
        for (size_t d = 0; d < wire.size(); ++d)
        {
            wire[d] += group_amp * amps1[w] * noise[d];
        }
    }
}
// ------------------------------------------------------

