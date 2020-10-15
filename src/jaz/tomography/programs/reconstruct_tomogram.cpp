#include "reconstruct_tomogram.h"
#include <src/jaz/tomography/projection/projection.h>
#include <src/jaz/tomography/extraction.h>
#include <src/jaz/tomography/reconstruction.h>
#include <src/jaz/tomography/tomogram.h>
#include <src/jaz/tomography/tomogram_set.h>
#include <src/jaz/image/normalization.h>
#include <src/jaz/image/centering.h>
#include <src/args.h>
#include <src/jaz/gravis/t4Matrix.h>

#include <omp.h>

using namespace gravis;


void TomoBackprojectProgram::readParameters(int argc, char *argv[])
{
	IOParser parser;
		
	try
	{
		parser.setCommandLine(argc, argv);
		int gen_section = parser.addSection("General options");
		
		tomoSetFn = parser.getOption("--t", "Tomogram set", "tomograms.star");
		tomoIndex = textToInteger(parser.getOption("--ti", "Tomogram index", "0"));
		
		applyWeight = parser.checkOption("--wg3D", "Perform weighting in Fourier space (using a Wiener filter)");
		applyPreWeight = parser.checkOption("--wg2D", "Pre-weight the 2D slices prior to backprojection");
		SNR = textToDouble(parser.getOption("--SNR", "SNR assumed by the Wiener filter", "10"));
		
		applyCtf = !parser.checkOption("--noctf", "Ignore the CTF");
		zeroDC = parser.checkOption("--0dc", "Zero the DC component of each frame");
		
		taperRad = textToDouble(parser.getOption("--td", "Tapering distance", "0.0"));

		thickness = textToInteger(parser.getOption("--th", "Thickness (read from .tlt file by default)", "-1"));
		
		x0 = textToDouble(parser.getOption("--x0", "X origin", "1.0"));
		y0 = textToDouble(parser.getOption("--y0", "Y origin", "1.0"));
		z0 = textToDouble(parser.getOption("--z0", "Z origin", "1.0"));
		
		w = textToInteger(parser.getOption("--w", "Width",  "-1.0"));
		h = textToInteger(parser.getOption("--h", "Height", "-1.0"));
		
		spacing = textToDouble(parser.getOption("--bin", "Binning (pixel spacing)", "8.0"));
		stack_spacing = textToDouble(parser.getOption("--stack_bin", "Binning level of the stack", "1.0"));	
		n_threads = textToInteger(parser.getOption("--j", "Number of threads", "1"));
		
		outFn = parser.getOption("--o", "Output filename");

		parser.checkForErrors();
	}
	catch (RelionError XE)
	{
		parser.writeUsage(std::cout);
		std::cerr << XE;
		exit(1);
	}
	
	if (applyPreWeight && applyWeight)
	{
		REPORT_ERROR("The options --wg3D and --wg2D are mutually exclusive.");
	}	
}

void TomoBackprojectProgram::run()
{
	TomogramSet tomogramSet(tomoSetFn);
	Tomogram tomogram = tomogramSet.loadTomogram(tomoIndex, true);
	
	const int w0 = tomogram.w0;
	const int h0 = tomogram.h0;
	
	std::cout << w0 << " x " << h0 << std::endl;
	
	if (thickness < 0)
	{
		thickness = tomogram.d0;
		std::cout << "Using thickness from '" << tomoSetFn << "': " << thickness << std::endl;
	}
	
	if (zeroDC) Normalization::zeroDC_stack(tomogram.stack);
		
	
	const int fc = tomogram.frameCount;
		
	BufferedImage<float> stackAct;
	std::vector<d4Matrix> projAct(fc);
	double pixelSizeAct = tomogram.optics.pixelSize;
	
	
	const int w1 = w > 0? w : w0 / spacing + 0.5;
	const int h1 = h > 0? h : h0 / spacing + 0.5;
	const int t1 = (int)(thickness/spacing);
	
	std::cout << w1 << "x" << h1 << "x" << t1 << std::endl; 
		
	if (std::abs(spacing - 1.0) < 1e-2)
	{
		projAct = tomogram.projectionMatrices;
		stackAct = tomogram.stack;
	}
	else
	{
		for (int f = 0; f < fc; f++)
		{
			projAct[f] = tomogram.projectionMatrices[f] / spacing;
			projAct[f](3,3) = 1.0;
		}
		
		if (std::abs(spacing / stack_spacing - 1.0) > 1e-2)
		{
			std::cout << "resampling image stack... " << std::endl;
			
			stackAct = Resampling::downsampleFiltStack_2D_full(
			            tomogram.stack, spacing / stack_spacing, n_threads);
			
			pixelSizeAct *= spacing / stack_spacing;
		}
		else
		{
			stackAct = tomogram.stack;
		}
	}
	
	const int w_stackAct = stackAct.xdim;
	const int h_stackAct = stackAct.ydim;
	const int wh_stackAct = w_stackAct/2 + 1;
	
	
	d3Vector orig(x0, y0, z0);
	BufferedImage<float> out(w1, h1, t1);
	out.fill(0.f);
	
	BufferedImage<float> psfStack;
	
	if (applyCtf)
	{
		// modulate stackAct with CTF (mind the spacing)
		
		psfStack.resize(w_stackAct, h_stackAct, fc);
		BufferedImage<fComplex> debug(wh_stackAct, h_stackAct, fc);
		
		#pragma omp parallel for num_threads(n_threads)
		for (int f = 0; f < fc; f++)
		{
			BufferedImage<float> frame = stackAct.getSliceRef(f);
			
			BufferedImage<fComplex> frameFS;
			FFT::FourierTransform(frame, frameFS, FFT::Both);
			
			CTF ctf = tomogram.centralCTFs[f];
			
			
			BufferedImage<fComplex> ctf2ImageFS(wh_stackAct, h_stackAct);
			
			const double box_size_x = pixelSizeAct * w_stackAct;
			const double box_size_y = pixelSizeAct * h_stackAct;
			
			for (int y = 0; y < h_stackAct;  y++)
			for (int x = 0; x < wh_stackAct; x++)
			{
				const double xA = x / box_size_x;
				const double yA = (y < h_stackAct/2? y : y - h_stackAct) / box_size_y;
				
				const float c = ctf.getCTF(xA, yA);
				
				ctf2ImageFS(x,y) = fComplex(c*c,0);
				frameFS(x,y) *= c;
			}
			
			FFT::inverseFourierTransform(frameFS, frame, FFT::Both);			
			stackAct.getSliceRef(f).copyFrom(frame);
			
			FFT::inverseFourierTransform(ctf2ImageFS, frame, FFT::Both);			
			psfStack.getSliceRef(f).copyFrom(frame);
			
			debug.getSliceRef(f).copyFrom(ctf2ImageFS);
		}
	}	
	
	if (applyPreWeight)
	{
		stackAct = RealSpaceBackprojection::preWeight(stackAct, projAct, n_threads);
	}
	
	
	std::cout << "backprojecting... " << std::endl;
	
	RealSpaceBackprojection::backproject(
			stackAct, projAct, out, n_threads, 
			orig, spacing, RealSpaceBackprojection::Linear, taperRad);
	
	
	if (applyWeight || applyCtf)
	{
		BufferedImage<float> psf(w1, h1, t1);
		psf.fill(0.f);
		
		if (applyCtf)
		{
			RealSpaceBackprojection::backproject(
					psfStack, projAct, psf, n_threads, 
					orig, spacing, RealSpaceBackprojection::Linear, taperRad);
		}
		else
		{
			RealSpaceBackprojection::backprojectPsf(
					stackAct, projAct, psf, n_threads, orig, spacing);
		}
		
		Reconstruction::correct3D_RS(out, psf, out, 1.0 / SNR, n_threads);
	}
	
	std::cout << "writing output..." << std::endl;
	
	const double samplingRate = tomogram.optics.pixelSize * spacing;

	out.write(outFn, samplingRate);
}
