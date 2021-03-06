#include <dlfcn.h>
#include "hcfftlib.h"


//  Static initialization of the repo lock variable
lockRAII FFTRepo::lockRepo( _T( "FFTRepo" ) );

//  Static initialization of the plan count variable
size_t FFTRepo::planCount = 1;
static size_t beforeCompile = 99999999;
static size_t countKernel, bakedPlanCount;
static std::string sfilename, skernellib;
static void* kernelHandle = NULL;

bool has_suffix(const std::string& s, const std::string& suffix) {
  return (s.size() >= suffix.size()) && equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

bool checkIfsoExist(hcfftDirection direction, hcfftPrecision precision, std::vector<size_t> originalLength, hcfftLibType hcfftlibtype) {
  DIR*           d;
  struct dirent* dir;
  std::string pwd = getHomeDir();
  bool soExist = false;
  pwd += "/kernCache/";
  d = opendir(pwd.c_str());

  if (d) {
    while ((dir = readdir(d)) != NULL) {
      if(has_suffix(dir->d_name, ".so")) {
        std::string libFile(dir->d_name);

        if(libFile.substr(0, 9) != "libkernel") {
          continue;
        }

        int i = 0;
        size_t length = libFile.length();
        size_t n = std::count(libFile.begin(), libFile.end(), '_');

        if((n - 1) != originalLength.size()) {
          continue;
        }

        size_t firstocc = libFile.find_first_of("_");
        std::string type = libFile.substr(9, 4);
        std::string datatype = libFile.substr(13, 1);
        std::string libtype = libFile.substr(14, 1);

        if(!((direction == HCFFT_FORWARD && type == "Frwd") || (direction == HCFFT_BACKWARD && type == "Back") || (direction == HCFFT_BOTH))) {
          continue;
        }

        if(!((datatype == "F" && precision == HCFFT_SINGLE)  || (datatype == "D" && precision == HCFFT_DOUBLE))) {
          continue;
        }

        if(!((libtype == "1" && hcfftlibtype == HCFFT_R2CD2Z) || (libtype == "2" && hcfftlibtype == HCFFT_C2RZ2D) || (libtype == "3" && hcfftlibtype == HCFFT_C2CZ2Z))) {
          continue;
        }

        if( firstocc != std::string::npos) {
          ++firstocc;
          size_t iter = (libFile.substr(firstocc, length - firstocc)).find("_");

          while( iter != std::string::npos) {
            size_t N = (size_t)stoi(libFile.substr(firstocc, iter));

            if(N != originalLength[i]) {
              break;
            }

            i++;
            firstocc  += iter + 1;
            iter = (libFile.substr(firstocc, length - firstocc )).find("_");
          }
        }

        if( i == originalLength.size()) {
          soExist = true;
          break;
        }
      }
    }

    if(closedir(d) < 0) {
      std::cout << " Directory failed to close " << std::endl;
      return false;
    }
  }

  return soExist ;
}

/*----------------------------------------------------FFTPlan-----------------------------------------------------------------------------*/

//  Read the kernels that this plan uses from file, and store into the plan
hcfftStatus WriteKernel( const hcfftPlanHandle plHandle, const hcfftGenerators gen, const FFTKernelGenKeyParams& fftParams, std::string filename, bool writeFlag) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  std::string kernel;
  fftRepo.getProgramCode( gen, plHandle, fftParams, kernel);
  FILE* fp;
  std::string pwd = getHomeDir();
  pwd += "/kernCache/";
  struct stat st = {0};

  if (stat(pwd.c_str(), &st) == -1) {
    mkdir(pwd.c_str(), 0777);
  }

  if(writeFlag) {
    fp = fopen (filename.c_str(), "w");

    if (!fp) {
      std::cout << " File kernel.cpp open failed for writing " << std::endl;
      return HCFFT_ERROR;
    }
  } else {
    fp = fopen (filename.c_str(), "a+");

    if (!fp) {
      std::cout << " File kernel.cpp open failed for writing " << std::endl;
      return HCFFT_ERROR;
    }
  }

  size_t written = fwrite(kernel.c_str(), kernel.size(), 1, fp);

  if(!written) {
    std::cout << "Kernel Write Failed " << std::endl;
    exit(1);
  }

  fflush(fp);
  fclose(fp);
  return  HCFFT_SUCCEEDS;
}

//  Compile the kernels that this plan uses, and store into the plan
hcfftStatus CompileKernels(const hcfftPlanHandle plHandle, const hcfftGenerators gen, FFTPlan* fftPlan, hcfftPlanHandle plHandleOrigin,
                           bool exist, std::vector<size_t> originalLength, hcfftLibType hcfftlibtype) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTKernelGenKeyParams fftParams;
  fftPlan->GetKernelGenKey( fftParams );
  // For real transforms we comppile either forward or backward kernel
  bool r2c_transform = (fftParams.fft_inputLayout == HCFFT_REAL);
  bool c2r_transform = (fftParams.fft_outputLayout == HCFFT_REAL);
  bool real_transform = (r2c_transform || c2r_transform);
  bool h2c = ((fftParams.fft_inputLayout == HCFFT_HERMITIAN_PLANAR) || (fftParams.fft_inputLayout == HCFFT_HERMITIAN_INTERLEAVED));
  bool c2h = ((fftParams.fft_outputLayout == HCFFT_HERMITIAN_PLANAR) || (fftParams.fft_outputLayout == HCFFT_HERMITIAN_INTERLEAVED));
  bool buildFwdKernel = (gen == Stockham || gen == Transpose_GCN || gen == Transpose_SQUARE || gen == Transpose_NONSQUARE) ? ((!real_transform) || r2c_transform) : (r2c_transform || c2h) || (!(h2c || c2h));
  bool buildBwdKernel = (gen == Stockham || gen == Transpose_GCN || gen == Transpose_SQUARE || gen == Transpose_NONSQUARE) ? ((!real_transform) || c2r_transform) : (c2r_transform || h2c) || (!(h2c || c2h));
  bool writeFlag = false;
  std::string type;

  if(buildFwdKernel) {
    type = "Frwd";
  }

  if (buildBwdKernel) {
    type = "Back";
  }

  if(fftParams.fft_precision == HCFFT_SINGLE) {
    type += "F";
  } else {
    type += "D";
  }

  if(hcfftlibtype == HCFFT_R2CD2Z) {
    type += "1_";
  } else if (hcfftlibtype == HCFFT_C2RZ2D) {
    type += "2_";
  } else {
    type += "3_";
  }

  if(beforeCompile != plHandleOrigin) {
    fftPlan->filename = getHomeDir();
    fftPlan->kernellib = fftPlan->filename;
    fftPlan->filename += "/kernCache/kernel";
    fftPlan->kernellib += "/kernCache/libkernel";
    fftPlan->filename += type;
    fftPlan->kernellib += type;

    for(int i = 0; i < originalLength.size(); i++) {
      fftPlan->filename += SztToStr(originalLength[i]);
      fftPlan->kernellib += SztToStr(originalLength[i]);
      fftPlan->filename += "_";
      fftPlan->kernellib += "_";
    }

    fftPlan->filename += ".cpp";
    fftPlan->kernellib += ".so";
    sfilename = fftPlan->filename;
    skernellib = fftPlan->kernellib;
    beforeCompile = plHandleOrigin;
    writeFlag = true;
  } else {
    fftPlan->filename = sfilename;
    fftPlan->kernellib = skernellib;
  }

  if(!exist) {
    WriteKernel( plHandle, gen, fftParams, fftPlan->filename, writeFlag);
    // Check if the default compiler path exists
    std::string execCmd;
    char fname[256] = "/opt/rocm/hcc/bin/clang++";

    if ( access ( getenv ("HCC_HOME"), F_OK ) != -1) {
      // TODO: This path shall be removed. User shall build from default path
      // compiler doesn't exist in default path
      // check if user has specified compiler build path
      // build_mode = true;
      char* compilerPath = getenv ("HCC_HOME");
      std::string Path(compilerPath);
      Path.append("/bin/");
      execCmd = Path + "clang++ `" + Path + "hcc-config --install --cxxflags --ldflags --shared` -Wno-unused-command-line-argument -lhc_am " + fftPlan->filename + " -o " + fftPlan->kernellib ;
    } else if( access( fname, F_OK ) != -1 ) {
      // compiler exists
      // install_mode = true;
      std::string Path = "/opt/rocm/hcc/bin/";
      execCmd = Path + "clang++ `" + Path + "hcc-config --install --cxxflags --ldflags --shared` -Wno-unused-command-line-argument " + fftPlan->filename + " -o " + fftPlan->kernellib ;
    } else {
      // No compiler found
      std::cout << "HCC compiler not found" << std::endl;
      return HCFFT_INVALID;
    }

    system(execCmd.c_str());
  }

  // get a kernel object handle for a kernel with the given name
  if(buildFwdKernel) {
    std::string entryPoint;
    fftRepo.getProgramEntryPoint( gen, plHandle, fftParams, HCFFT_FORWARD, entryPoint);
  }

  if(buildBwdKernel) {
    std::string entryPoint;
    fftRepo.getProgramEntryPoint( gen, plHandle, fftParams, HCFFT_BACKWARD, entryPoint);
  }

  return HCFFT_SUCCEEDS;
}

//  This routine will query the OpenCL context for it's devices
//  and their hardware limitations, which we synthesize into a
//  hardware "envelope".
//  We only query the devices the first time we're called after
//  the object's context is set.  On 2nd and subsequent calls,
//  we just return the pointer.
//
hcfftStatus FFTPlan::SetEnvelope () {
  // TODO  The caller has already acquired the lock on *this
  //  However, we shouldn't depend on it.
  envelope.limit_LocalMemSize = 32768;
  envelope.limit_WorkGroupSize = 256;
  envelope.limit_Dimensions = 3;

  for(int i = 0 ; i < envelope.limit_Dimensions; i++) {
    envelope.limit_Size[i] = 256;
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::GetEnvelope (const FFTEnvelope** ppEnvelope) const {
  *ppEnvelope = &envelope;
  return HCFFT_SUCCEEDS;
}

hcfftStatus hcfftCreateDefaultPlanInternal (hcfftPlanHandle* plHandle, hcfftDim dimension, const size_t* length) {
  if( length == NULL ) {
    return HCFFT_ERROR;
  }

  size_t lenX = 1, lenY = 1, lenZ = 1;

  switch( dimension ) {
    case HCFFT_1D: {
        if( length[ 0 ] == 0 ) {
          return HCFFT_ERROR;
        }

        lenX = length[ 0 ];
      }
      break;

    case HCFFT_2D: {
        if( length[ 0 ] == 0 || length[ 1 ] == 0 ) {
          return HCFFT_ERROR;
        }

        lenX = length[ 0 ];
        lenY = length[ 1 ];
      }
      break;

    case HCFFT_3D: {
        if( length[ 0 ] == 0 || length[ 1 ] == 0 || length[ 2 ] == 0 ) {
          return HCFFT_ERROR;
        }

        lenX = length[ 0 ];
        lenY = length[ 1 ];
        lenZ = length[ 2 ];
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  FFTPlan* fftPlan = NULL;
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  fftRepo.createPlan( plHandle, fftPlan );
  fftPlan->baked = false;
  fftPlan->dimension = dimension;
  fftPlan->location = HCFFT_INPLACE;
  fftPlan->ipLayout = HCFFT_COMPLEX_INTERLEAVED;
  fftPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
  fftPlan->precision = HCFFT_SINGLE;
  fftPlan->forwardScale = 1.0;
  fftPlan->backwardScale = 1.0 / static_cast< double >( lenX * lenY * lenZ );
  fftPlan->batchSize = 1;
  fftPlan->gen = Stockham; //default setting
  fftPlan->SetEnvelope();
  //  Need to devise a way to generate better names
  std::stringstream tstream;
  tstream << _T( "plan_" ) << *plHandle;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( *plHandle, fftPlan, planLock );
  planLock->setName( tstream.str( ) );

  switch( dimension ) {
    case HCFFT_1D: {
        fftPlan->length.push_back( lenX );
        fftPlan->inStride.push_back( 1 );
        fftPlan->outStride.push_back( 1 );
        fftPlan->iDist    = lenX;
        fftPlan->oDist    = lenX;
      }
      break;

    case HCFFT_2D: {
        fftPlan->length.push_back( lenX );
        fftPlan->length.push_back( lenY );
        fftPlan->inStride.push_back( 1 );
        fftPlan->inStride.push_back( lenX );
        fftPlan->outStride.push_back( 1 );
        fftPlan->outStride.push_back( lenX );
        fftPlan->iDist    = lenX * lenY;
        fftPlan->oDist    = lenX * lenY;
      }
      break;

    case HCFFT_3D: {
        fftPlan->length.push_back( lenX );
        fftPlan->length.push_back( lenY );
        fftPlan->length.push_back( lenZ );
        fftPlan->inStride.push_back( 1 );
        fftPlan->inStride.push_back( lenX );
        fftPlan->inStride.push_back( lenX * lenY );
        fftPlan->outStride.push_back( 1 );
        fftPlan->outStride.push_back( lenX );
        fftPlan->outStride.push_back( lenX * lenY );
        fftPlan->iDist    = lenX * lenY * lenZ;
        fftPlan->oDist    = lenX * lenY * lenZ;
      }
      break;
  }

  fftPlan->plHandle = *plHandle;
  return HCFFT_SUCCEEDS;
}

// This external entry-point should not be called from within the library. Use hcfftCreateDefaultPlanInternal instead.
hcfftStatus FFTPlan::hcfftCreateDefaultPlan( hcfftPlanHandle* plHandle, const hcfftDim dim,
    const size_t* hcLengths, hcfftDirection dir, hcfftPrecision precision,
    hcfftLibType libType) {
  hcfftStatus ret = hcfftCreateDefaultPlanInternal(plHandle, dim, hcLengths);

  if(ret == HCFFT_SUCCEEDS) {
    FFTRepo& fftRepo  = FFTRepo::getInstance( );
    FFTPlan* fftPlan = NULL;
    lockRAII* planLock  = NULL;
    fftRepo.getPlan( *plHandle, fftPlan, planLock );
    fftPlan->direction = dir;
    fftPlan->plHandleOrigin = *plHandle;
    fftPlan->originalLength.clear();

    for(int i = 0 ; i < dim ; i++) {
      fftPlan->originalLength.push_back(hcLengths[i]);
    }

    fftPlan->userPlan = true;
    fftPlan->hcfftlibtype = libType;
  }

  return ret;
}

hcfftStatus FFTPlan::hcfftSetAcclView( hcfftPlanHandle plHandle, hc::accelerator_view acc_view) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftSetAcclView" ) );
  fftPlan->acc_view = acc_view;
  fftPlan->acc = acc_view.get_accelerator();
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetAcclView( hcfftPlanHandle plHandle, hc::accelerator_view* acc_view) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftGetAcclView" ) );
  *acc_view = fftPlan->acc_view;
  return HCFFT_SUCCEEDS;
}

template<typename T>
hcfftStatus FFTPlan::hcfftEnqueueTransform(hcfftPlanHandle plHandle, hcfftDirection dir, T* hcInputBuffers,
    T* hcOutputBuffers, T* hcTmpBuffers) {
  hcfftStatus status = HCFFT_INVALID;
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftEnqueueTransform" ) );
  countKernel = 0;

  if(fftPlan->transformed == false) {
    char* err = (char*) calloc(128, 2);
    kernelHandle = dlopen(fftPlan->kernellib.c_str(), RTLD_NOW);

    if(!kernelHandle) {
      std::cout << "Failed to load Kernel: " << fftPlan->kernellib.c_str() << std::endl;
      return HCFFT_ERROR;
    }
  }

  status = hcfftEnqueueTransformInternal<T>(plHandle, dir, hcInputBuffers, hcOutputBuffers, hcTmpBuffers);
  remove(fftPlan->filename.c_str());
  fftPlan->transformed = true;
  return status;
}

// Template Initialization
template hcfftStatus FFTPlan::hcfftEnqueueTransform(hcfftPlanHandle plHandle, hcfftDirection dir, float* hcInputBuffers, float* hcOutputBuffers, float* hcTmpBuffers);
template hcfftStatus FFTPlan::hcfftEnqueueTransform(hcfftPlanHandle plHandle, hcfftDirection dir, double* hcInputBuffers, double* hcOutputBuffers, double* hcTmpBuffers);

template<typename T>
hcfftStatus FFTPlan::hcfftEnqueueTransformInternal(hcfftPlanHandle plHandle, hcfftDirection dir, T* hcInputBuffers,
    T* hcOutputBuffers, T* hcTmpBuffers) {
  hcfftStatus status = HCFFT_SUCCEEDS;
  std::map<int, void*> vectArr;
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  //  At this point, the user wants to enqueue a plan to execute.  We lock the plan down now, such that
  //  after we finish baking the plan (if the user did not do that explicitely before), the plan cannot
  //  change again through the action of other thread before we enqueue this plan for execution.
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( "hcfftEnqueueTransformInternal" ) );

  if( fftPlan->baked == false ) {
    hcfftBakePlan( plHandle);
  }

  if (fftPlan->ipLayout == HCFFT_REAL) {
    dir = HCFFT_FORWARD;
  } else if (fftPlan->opLayout == HCFFT_REAL) {
    dir = HCFFT_BACKWARD;
  }

  if( hcTmpBuffers == NULL && fftPlan->tmpBufSize > 0 && fftPlan->intBuffer == NULL) {
    // create the intermediate buffers
    // The intermediate buffer is always interleave and packed
    // For outofplace operation, we have the choice not to create intermediate buffer
    // input ->(col+Transpose) output ->(col) output
    fftPlan->intBuffer = hc::am_alloc(fftPlan->tmpBufSize, fftPlan->acc , 0);

    if(fftPlan->intBuffer == NULL) {
      return HCFFT_INVALID;
    }
  }

  if( hcTmpBuffers == NULL && fftPlan->intBuffer != NULL ) {
    hcTmpBuffers = (T*)fftPlan->intBuffer;
  }

  if( fftPlan->intBufferRC == NULL && fftPlan->tmpBufSizeRC > 0 ) {
    fftPlan->intBufferRC = hc::am_alloc(fftPlan->tmpBufSizeRC, fftPlan->acc , 0);

    if(fftPlan->intBufferRC == NULL) {
      return HCFFT_INVALID;
    }
  }

  if( fftPlan->intBufferC2R == NULL && fftPlan->tmpBufSizeC2R > 0 ) {
    fftPlan->intBufferC2R = hc::am_alloc(fftPlan->tmpBufSizeC2R, fftPlan->acc , 0);

    if(fftPlan->intBufferC2R == NULL) {
      return HCFFT_INVALID;
    }
  }

  //  The largest vector we can transform in a single pass
  //  depends on the GPU caps -- especially the amount of LDS
  //  available
  //
  size_t Large1DThreshold = 0;
  fftPlan->GetMax1DLength (&Large1DThreshold);
  BUG_CHECK (Large1DThreshold > 1);

  if(fftPlan->gen != Copy)
    switch( fftPlan->dimension ) {
      case HCFFT_1D: {
          if ( Is1DPossible(fftPlan->length[0], Large1DThreshold) ) {
            break;
          }

          if(  ( fftPlan->ipLayout == HCFFT_REAL ) && ( fftPlan->planTZ != 0) ) {
            //First transpose
            // Input->tmp
            hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, hcTmpBuffers, NULL);

            if (fftPlan->location == HCFFT_INPLACE) {
              //First Row
              //tmp->output
              hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcTmpBuffers, (T*)fftPlan->intBufferRC, NULL );
              //Second Transpose
              // output->tmp
              hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, (T*)fftPlan->intBufferRC, hcTmpBuffers, NULL );
              //Second Row
              //tmp->tmp, inplace
              hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, (T*)fftPlan->intBufferRC, NULL );
              //Third Transpose
              // tmp->output
              hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, (T*)fftPlan->intBufferRC, hcInputBuffers, NULL );
            } else {
              //First Row
              //tmp->output
              hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcTmpBuffers, (T*)fftPlan->intBufferRC, NULL );
              //Second Transpose
              // output->tmp
              hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, (T*)fftPlan->intBufferRC, hcTmpBuffers, NULL );
              //Second Row
              //tmp->tmp, inplace
              hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, (T*)fftPlan->intBufferRC, NULL );
              //Third Transpose
              // tmp->output
              hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, (T*)fftPlan->intBufferRC, hcOutputBuffers, NULL );
            }
          } else if ( fftPlan->ipLayout == HCFFT_REAL ) {
            // First pass
            // column with twiddle first, OUTOFPLACE, + transpose
            hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_FORWARD, hcInputBuffers, (T*)fftPlan->intBufferRC, hcTmpBuffers);
            // another column FFT output, INPLACE
            hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_FORWARD, (T*)fftPlan->intBufferRC, (T*)fftPlan->intBufferRC, hcTmpBuffers );

            if(fftPlan->location == HCFFT_INPLACE) {
              // copy from full complex to hermitian
              hcfftEnqueueTransformInternal<T>( fftPlan->planRCcopy, HCFFT_FORWARD, (T*)fftPlan->intBufferRC, hcInputBuffers, hcTmpBuffers );
            } else {
              hcfftEnqueueTransformInternal<T>( fftPlan->planRCcopy, HCFFT_FORWARD, (T*)fftPlan->intBufferRC, hcOutputBuffers, hcTmpBuffers );
            }
          } else if( fftPlan->opLayout == HCFFT_REAL ) {
            if (fftPlan->planRCcopy) {
              // copy from hermitian to full complex
              hcfftEnqueueTransformInternal<T>(fftPlan->planRCcopy, HCFFT_BACKWARD, hcInputBuffers, (T*)fftPlan->intBufferRC, hcTmpBuffers);
              // First pass
              // column with twiddle first, INPLACE,
              hcfftEnqueueTransformInternal<T>(fftPlan->planX, HCFFT_BACKWARD, (T*)fftPlan->intBufferRC, (T*)fftPlan->intBufferRC, hcTmpBuffers);
            } else {
              // First pass
              // column with twiddle first, INPLACE,
              hcfftEnqueueTransformInternal<T>(fftPlan->planX, HCFFT_BACKWARD, hcInputBuffers, (T*)fftPlan->intBufferRC, hcTmpBuffers);
            }

            if(fftPlan->location == HCFFT_INPLACE) {
              // another column FFT output, OUTOFPLACE + transpose
              hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_BACKWARD, (T*)fftPlan->intBufferRC, hcInputBuffers, hcTmpBuffers );
            } else {
              hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_BACKWARD, (T*)fftPlan->intBufferRC, hcOutputBuffers, hcTmpBuffers );
            }
          } else {
            if (fftPlan->transflag) {
              if(fftPlan->allOpsInplace) {
                //First transpose
                // Input->tmp
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, NULL, NULL );
              } else {
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, hcTmpBuffers, NULL );
              }

              if (fftPlan->location == HCFFT_INPLACE) {
                //First Row
                //tmp->output
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcTmpBuffers, hcInputBuffers, NULL );
                }

                //Second Transpose
                // output->tmp
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcInputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcInputBuffers, hcTmpBuffers, NULL );
                }

                //Second Row
                //tmp->tmp, inplace
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcInputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                }

                //Third Transpose
                // tmp->output
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcInputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcTmpBuffers, hcInputBuffers, NULL );
                }
              } else {
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcOutputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                }

                //Second Transpose
                // output->tmp
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcOutputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcOutputBuffers, hcTmpBuffers, NULL );
                }

                //Second Row
                //tmp->tmp, inplace
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcOutputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                }

                //Third Transpose
                // tmp->output
                if(fftPlan->allOpsInplace) {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcOutputBuffers, NULL, NULL );
                } else {
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                }
              }
            } else {
              if (fftPlan->large1D == 0) {
                if(fftPlan->planCopy) {
                  // Transpose OUTOFPLACE
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, hcTmpBuffers, NULL ),
                                                 // FFT INPLACE
                                                 hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcTmpBuffers, NULL, NULL);
                  // FFT INPLACE
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );

                  if (fftPlan->location == HCFFT_INPLACE) {
                    // Copy kernel
                    hcfftEnqueueTransformInternal<T>( fftPlan->planCopy, dir, hcTmpBuffers, hcInputBuffers, NULL );
                  } else {
                    // Copy kernel
                    hcfftEnqueueTransformInternal<T>( fftPlan->planCopy, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                  }
                } else {
                  // First pass
                  // column with twiddle first, OUTOFPLACE, + transpose
                  hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcTmpBuffers, NULL);

                  if(fftPlan->planTZ) {
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );

                    if (fftPlan->location == HCFFT_INPLACE) {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcTmpBuffers, hcInputBuffers, NULL );
                    } else {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                    }
                  } else {
                    //another column FFT output, OUTOFPLACE
                    if (fftPlan->location == HCFFT_INPLACE) {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, hcInputBuffers, NULL );
                    } else {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                    }
                  }
                }
              } else {
                // second pass for huge 1D
                // column with twiddle first, OUTOFPLACE, + transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcTmpBuffers, hcOutputBuffers, hcTmpBuffers);
                hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcOutputBuffers, hcOutputBuffers, hcTmpBuffers );
              }
            }
          }

          return  HCFFT_SUCCEEDS;
        }

      case HCFFT_2D: {
          // if transpose kernel, we will fall below
          if (fftPlan->transflag && !(fftPlan->planTX)) {
            break;
          }

          if ( (fftPlan->gen == Transpose_NONSQUARE ) &&
               (fftPlan->nonSquareKernelType == NON_SQUARE_TRANS_PARENT) ) {
            hcfftEnqueueTransformInternal<T>(fftPlan->planTX, dir, hcInputBuffers, NULL, NULL);
            hcfftEnqueueTransformInternal<T>(fftPlan->planTY, dir, hcInputBuffers, NULL, NULL);
            return  HCFFT_SUCCEEDS;
          }

          if (fftPlan->transflag) {
            //first time set up transpose kernel for 2D
            //First row
            hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcOutputBuffers, NULL );

            if (fftPlan->location == HCFFT_INPLACE) {
              if (!fftPlan->transpose_in_2d_inplace) {
                //First transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, hcTmpBuffers, NULL );

                if (fftPlan->transposeType == HCFFT_NOTRANSPOSE) {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                  //Second transpose
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcTmpBuffers, hcInputBuffers, NULL );
                } else {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, hcInputBuffers, NULL );
                }
              } else {
                // First Transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, NULL, NULL );

                if (fftPlan->transposeType == HCFFT_NOTRANSPOSE) {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcInputBuffers, NULL, NULL );
                  //Second transpose
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcInputBuffers, NULL, NULL );
                } else {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcInputBuffers, NULL, NULL );
                }
              }
            } else {
              if (!fftPlan->transpose_in_2d_inplace) {
                //First transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcOutputBuffers, hcTmpBuffers, NULL );

                if (fftPlan->transposeType == HCFFT_NOTRANSPOSE) {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                  //Second transpose
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                } else {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                }
              } else {
                // First Transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcOutputBuffers, NULL, NULL );

                if (fftPlan->transposeType == HCFFT_NOTRANSPOSE) {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcOutputBuffers, NULL, NULL );
                  //Second transpose
                  hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcOutputBuffers, NULL, NULL );
                } else {
                  //Second Row transform
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcOutputBuffers, NULL, NULL );
                }
              }
            }
          } else {
            if ( (fftPlan->large2D || fftPlan->length.size() > 2) &&
                 (fftPlan->ipLayout != HCFFT_REAL) && (fftPlan->opLayout != HCFFT_REAL)) {
              if (fftPlan->location == HCFFT_INPLACE) {
                //deal with row first
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, NULL, hcTmpBuffers );
                //deal with column
                hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcInputBuffers, NULL, hcTmpBuffers );
              } else {
                //deal with row first
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcOutputBuffers, hcTmpBuffers );
                //deal with column
                hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcOutputBuffers, NULL, hcTmpBuffers );
              }
            } else {
              if(fftPlan->ipLayout == HCFFT_REAL) {
                if(fftPlan->planTX) {
                  //First row
                  hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcOutputBuffers, NULL );

                  if (fftPlan->location == HCFFT_INPLACE) {
                    //First transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, hcTmpBuffers, NULL );
                    //Second Row transform
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                    //Second transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcTmpBuffers, hcInputBuffers, NULL );
                  } else {
                    //First transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcOutputBuffers, hcTmpBuffers, NULL );
                    //Second Row transform
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                    //Second transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                  }
                } else {
                  if (fftPlan->location == HCFFT_INPLACE) {
                    // deal with row
                    hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_FORWARD, hcInputBuffers, NULL, hcTmpBuffers );
                    // deal with column
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_FORWARD, hcInputBuffers, NULL, hcTmpBuffers );
                  } else {
                    // deal with row
                    hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_FORWARD, hcInputBuffers, hcOutputBuffers, hcTmpBuffers );
                    // deal with column
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_FORWARD, hcOutputBuffers, NULL, hcTmpBuffers );
                  }
                }
              } else if(fftPlan->opLayout == HCFFT_REAL) {
// NOTE : 2D C2R CALL COMES HERE
                if(fftPlan->planTY) {
                  if ( (fftPlan->location == HCFFT_INPLACE) ||
                       ((fftPlan->location == HCFFT_OUTOFPLACE) && (fftPlan->length.size() > 2)) ) {
                    //First transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcInputBuffers, hcTmpBuffers, NULL );
                    //First row
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                    //Second transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcTmpBuffers, hcInputBuffers, NULL );

                    //Second Row transform
                    if(fftPlan->location == HCFFT_INPLACE) {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, NULL, NULL );
                    } else {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcOutputBuffers, NULL );
                    }
                  } else {
                    //First transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcInputBuffers, hcTmpBuffers, NULL );
                    //First row
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, NULL, NULL );
                    //Second transpose
                    hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcTmpBuffers, (T*)fftPlan->intBufferC2R, NULL );

                    //Second Row transform
                    if(fftPlan->location == HCFFT_INPLACE) {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, NULL, NULL );
                    } else {
                      hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, (T*)fftPlan->intBufferC2R, hcOutputBuffers, NULL );
                    }
                  }
                } else {
                  if(fftPlan->location == HCFFT_INPLACE) {
                    // deal with column
                    hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_BACKWARD, hcInputBuffers, NULL, hcTmpBuffers );
                    // deal with row
                    hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_BACKWARD, hcInputBuffers, NULL, hcTmpBuffers );
                  } else {
                    if(fftPlan->length.size() > 2) {
                      // deal with column
                      hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_BACKWARD, hcInputBuffers, NULL, hcTmpBuffers );
                      // deal with row
                      hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_BACKWARD, hcInputBuffers, hcOutputBuffers, hcTmpBuffers );
                    } else {
                      // deal with column
                      hcfftEnqueueTransformInternal<T>( fftPlan->planY, HCFFT_BACKWARD, hcInputBuffers, (T*)fftPlan->intBufferC2R, hcTmpBuffers );
                      // deal with row
                      hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_BACKWARD, (T*)fftPlan->intBufferC2R, hcOutputBuffers, hcTmpBuffers );
                    }
                  }
                }
              } else {
                //deal with row first
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcTmpBuffers, NULL );

                if (fftPlan->location == HCFFT_INPLACE) {
                  //deal with column
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, hcInputBuffers, NULL );
                } else {
                  //deal with column
                  hcfftEnqueueTransformInternal<T>( fftPlan->planY, dir, hcTmpBuffers, hcOutputBuffers, NULL );
                }
              }
            }
          }

          return HCFFT_SUCCEEDS;
        }

      case HCFFT_3D: {
          if(fftPlan->ipLayout == HCFFT_REAL) {
            if(fftPlan->planTX) {
              //First row
              hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcOutputBuffers, hcTmpBuffers);

              if (fftPlan->location == HCFFT_INPLACE) {
                //First transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcInputBuffers, hcTmpBuffers, NULL );
                //Second Row transform
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, dir, hcTmpBuffers, NULL, NULL );
                //Second transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcTmpBuffers, hcInputBuffers, NULL );
              } else {
                //First transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcOutputBuffers, hcTmpBuffers, NULL );
                //Second Row transform
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, dir, hcTmpBuffers, NULL, NULL );
                //Second transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTY, dir, hcTmpBuffers, hcOutputBuffers, NULL );
              }
            } else {
              if(fftPlan->location == HCFFT_INPLACE) {
                //deal with 2D row first
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_FORWARD, hcInputBuffers, NULL, hcTmpBuffers );
                //deal with 1D Z column
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, HCFFT_FORWARD, hcInputBuffers, NULL, hcTmpBuffers );
              } else {
                //deal with 2D row first
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_FORWARD, hcInputBuffers, hcOutputBuffers, hcTmpBuffers );
                //deal with 1D Z column
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, HCFFT_FORWARD, hcOutputBuffers, NULL, hcTmpBuffers );
              }
            }
          } else if(fftPlan->opLayout == HCFFT_REAL) {
            if(fftPlan->planTZ) {
              if(fftPlan->location == HCFFT_INPLACE) {
                //First transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcInputBuffers, hcTmpBuffers, NULL );
                //First row
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, dir, hcTmpBuffers, NULL, NULL );
                //Second transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcTmpBuffers, hcInputBuffers, NULL );
                //Second Row transform
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, NULL, NULL );
              } else {
                //First transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTZ, dir, hcInputBuffers, hcTmpBuffers, NULL );
                //First row
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, dir, hcTmpBuffers, NULL, NULL );
                //Second transpose
                hcfftEnqueueTransformInternal<T>( fftPlan->planTX, dir, hcTmpBuffers, (T*)fftPlan->intBufferC2R, NULL );
                //Second Row transform
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, (T*)fftPlan->intBufferC2R, hcOutputBuffers, NULL );
              }
            } else {
              if(fftPlan->location == HCFFT_INPLACE) {
                //deal with 1D Z column first
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, HCFFT_BACKWARD, hcInputBuffers, NULL, hcTmpBuffers );
                //deal with 2D row
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_BACKWARD, hcInputBuffers, NULL, hcTmpBuffers );
              } else {
                //deal with 1D Z column first
                hcfftEnqueueTransformInternal<T>( fftPlan->planZ, HCFFT_BACKWARD, hcInputBuffers, (T*)fftPlan->intBufferC2R, hcTmpBuffers );
                //deal with 2D row
                hcfftEnqueueTransformInternal<T>( fftPlan->planX, HCFFT_BACKWARD, (T*)fftPlan->intBufferC2R, hcOutputBuffers, hcTmpBuffers );
              }
            }
          } else {
            if (fftPlan->location == HCFFT_INPLACE) {
              //deal with 2D row first
              hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, NULL, hcTmpBuffers );
              //deal with 1D Z column
              hcfftEnqueueTransformInternal<T>( fftPlan->planZ, dir, hcInputBuffers, NULL, hcTmpBuffers );
            } else {
              //deal with 2D row first
              hcfftEnqueueTransformInternal<T>( fftPlan->planX, dir, hcInputBuffers, hcOutputBuffers, hcTmpBuffers );
              //deal with 1D Z column
              hcfftEnqueueTransformInternal<T>( fftPlan->planZ, dir, hcOutputBuffers, NULL, hcTmpBuffers );
            }
          }

          return HCFFT_SUCCEEDS;
        }
    }

  FFTKernelGenKeyParams fftParams;
  //  Translate the user plan into the structure that we use to map plans to hcPrograms
  fftPlan->GetKernelGenKey( fftParams );
  std::string kernel;
  fftRepo.getProgramCode( fftPlan->gen, plHandle, fftParams, kernel);
  unsigned int uarg = 0;
  uint batch = std::max<uint> (1, uint(fftPlan->batchSize));

  //  Decode the relevant properties from the plan paramter to figure out how many input/output buffers we have
  switch( fftPlan->ipLayout ) {
    case HCFFT_COMPLEX_INTERLEAVED: {
        switch( fftPlan->opLayout ) {
          case HCFFT_COMPLEX_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_COMPLEX_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                //  Invalid to be an inplace transform, and go from 1 to 2 buffers
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_REAL: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          default: {
              //  Don't recognize output layout
              return HCFFT_ERROR;
            }
        }

        break;
      }

    case HCFFT_COMPLEX_PLANAR: {
        switch( fftPlan->opLayout ) {
          case HCFFT_COMPLEX_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_COMPLEX_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_REAL: {
              if(fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          default: {
//  Don't recognize output layout
              return HCFFT_ERROR;
            }
        }

        break;
      }

    case HCFFT_HERMITIAN_INTERLEAVED: {
        switch( fftPlan->opLayout ) {
          case HCFFT_COMPLEX_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_COMPLEX_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_INTERLEAVED: {
              return HCFFT_ERROR;
            }

          case HCFFT_HERMITIAN_PLANAR: {
              return HCFFT_ERROR;
            }

          case HCFFT_REAL: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          default: {
              //  Don't recognize output layout
              return HCFFT_ERROR;
            }
        }

        break;
      }

    case HCFFT_HERMITIAN_PLANAR: {
        switch( fftPlan->opLayout ) {
          case HCFFT_COMPLEX_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_COMPLEX_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_INTERLEAVED: {
              return HCFFT_ERROR;
            }

          case HCFFT_HERMITIAN_PLANAR: {
              return HCFFT_ERROR;
            }

          case HCFFT_REAL: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          default: {
              //  Don't recognize output layout
              return HCFFT_ERROR;
            }
        }

        break;
      }

    case HCFFT_REAL: {
        switch( fftPlan->opLayout ) {
          case HCFFT_COMPLEX_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_COMPLEX_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_INTERLEAVED: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          case HCFFT_HERMITIAN_PLANAR: {
              if( fftPlan->location == HCFFT_INPLACE ) {
                return HCFFT_ERROR;
              } else {
                vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
              }

              break;
            }

          default: {
              if(fftPlan->transflag) {
                if( fftPlan->location == HCFFT_INPLACE ) {
                  return HCFFT_ERROR;
                } else {
                  vectArr.insert(std::make_pair(uarg++, hcInputBuffers));
                  vectArr.insert(std::make_pair(uarg++, hcOutputBuffers));
                }
              } else {
                //  Don't recognize output layout
                return HCFFT_ERROR;
              }
            }
        }

        break;
      }

    default: {
        //  Don't recognize output layout
        return HCFFT_ERROR;
      }
  }

  if(fftPlan->gen == Stockham || fftPlan->gen == Transpose_GCN || fftPlan->gen == Transpose_SQUARE || fftPlan->gen == Transpose_NONSQUARE) {
    if(fftPlan->twiddles != NULL) {
      vectArr.insert(std::make_pair(uarg++, fftPlan->twiddles));
    }

    if(fftPlan->twiddleslarge != NULL) {
      vectArr.insert(std::make_pair(uarg++, fftPlan->twiddleslarge));
    }
  }

  if(fftPlan->transformed == false ) {
    typedef void (FUNC_FFTFwd)(std::map<int, void*>* vectArr, uint batchSize, hc::accelerator_view & acc_view, hc::accelerator & acc);
    FUNC_FFTFwd* FFTcall = NULL;

    if(fftPlan->gen == Copy) {
      bool h2c = ((fftPlan->ipLayout == HCFFT_HERMITIAN_PLANAR) ||
                  (fftPlan->ipLayout == HCFFT_HERMITIAN_INTERLEAVED) ) ? true : false;
      std::string funcName = "copy_";

      if(h2c) {
        funcName += "h2c";
      } else {
        funcName += "c2h";
      }

      funcName +=  std::to_string(countKernel);
      FFTcall = (FUNC_FFTFwd*) dlsym(kernelHandle, funcName.c_str());

      if (!FFTcall) {
        std::cout << "Loading copy() fails " << std::endl;
      }

      char* err = dlerror();

      if (err) {
        std::cout << "failed to locate copy(): " << err;
        exit(1);
      }

      free(err);
    } else if(fftPlan->gen == Stockham) {
      if(dir == HCFFT_FORWARD) {
        std::string funcName = "fft_fwd";
        funcName +=  std::to_string(countKernel);
        FFTcall = (FUNC_FFTFwd*) dlsym(kernelHandle, funcName.c_str());

        if (!FFTcall) {
          std::cout << "Loading fft_fwd fails " << std::endl;
        }

        char* err = dlerror();

        if (err) {
          std::cout << "failed to locate fft_fwd(): " << err;
          exit(1);
        }

        free(err);
      } else if(dir == HCFFT_BACKWARD) {
        std::string funcName = "fft_back";
        funcName +=  std::to_string(countKernel);
        FFTcall = (FUNC_FFTFwd*) dlsym(kernelHandle, funcName.c_str());

        if (!FFTcall) {
          std::cout << "Loading fft_back fails " << std::endl;
        }

        char* err = dlerror();

        if (err) {
          std::cout << "failed to locate fft_back(): " << err;
          exit(1);
        }

        free(err);
      }
    } else if(fftPlan->gen == Transpose_GCN) {
      std::string funcName;

      if( fftParams.fft_3StepTwiddle ) {
        funcName = "transpose_gcn_tw_fwd";
      } else {
        funcName = "transpose_gcn";
      }

      funcName +=  std::to_string(countKernel);
      FFTcall = (FUNC_FFTFwd*) dlsym(kernelHandle, funcName.c_str());

      if (!FFTcall) {
        std::cout << "Loading transpose_gcn fails " << std::endl;
      }

      char* err = dlerror();

      if (err) {
        std::cout << "failed to locate transpose_gcn(): " << err;
        exit(1);
      }

      free(err);
    } else if(fftPlan->gen == Transpose_SQUARE) {
      std::string funcName;

      if( fftParams.fft_3StepTwiddle ) {
        funcName = "transpose_square_tw_fwd";
      } else {
        funcName = "transpose_square";
      }

      funcName +=  std::to_string(countKernel);
      FFTcall = (FUNC_FFTFwd*) dlsym(kernelHandle, funcName.c_str());

      if (!FFTcall) {
        std::cout << "Loading transpose_square fails " << std::endl;
      }

      char* err = dlerror();

      if (err) {
        std::cout << "failed to locate transpose_square(): " << err;
        exit(1);
      }

      free(err);
    } else if(fftPlan->gen == Transpose_NONSQUARE) {
      std::string funcName = "transpose_nonsquare";

      if (fftParams.nonSquareKernelType == NON_SQUARE_TRANS_TRANSPOSE_BATCHED_LEADING) {
        if (fftParams.fft_3StepTwiddle) {
          funcName = "transpose_nonsquare_tw_fwd";
        } else {
          funcName = "transpose_nonsquare";
        }
      } else if(fftParams.nonSquareKernelType == NON_SQUARE_TRANS_TRANSPOSE_BATCHED) {
        funcName = "transpose_square";
      }

      funcName +=  std::to_string(countKernel);
      FFTcall = (FUNC_FFTFwd*) dlsym(kernelHandle, funcName.c_str());

      if (!FFTcall) {
        std::cout << "Loading transpose_nonsquare fails " << std::endl;
      }

      char* err = dlerror();

      if (err) {
        std::cout << "failed to locate transpose_nonsquare(): " << err;
        exit(1);
      }

      free(err);
    }

    fftPlan->kernelPtr = FFTcall;
    fftPlan->transformed = true;
  }

  std::vector< size_t > gWorkSize;
  std::vector< size_t > lWorkSize;
  hcfftStatus result = fftPlan->GetWorkSizes (gWorkSize, lWorkSize);

  if (HCFFT_ERROR == result) {
    std::cout << "Work size too large for EnqueueTransform" << std::endl;
  }

  BUG_CHECK (gWorkSize.size() == lWorkSize.size());
  fftPlan->kernelPtr(&vectArr, batch, fftPlan->acc_view, fftPlan->acc);
  countKernel++;
  return status;
}
// Template Initialization supporting just float and double types
template hcfftStatus FFTPlan::hcfftEnqueueTransformInternal<float>(hcfftPlanHandle plHandle, hcfftDirection dir, float* hcInputBuffers, float* hcOutputBuffers, float* hcTmpBuffers); 
template hcfftStatus FFTPlan::hcfftEnqueueTransformInternal<double>(hcfftPlanHandle plHandle, hcfftDirection dir, double* hcInputBuffers, double* hcOutputBuffers, double* hcTmpBuffers); 


hcfftStatus FFTPlan::hcfftBakePlan(hcfftPlanHandle plHandle) {
  bakedPlanCount = 0;
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( "hcfftBakePlan" ) );

  if(fftPlan->baked == true) {
    return HCFFT_SUCCEEDS;
  }

  fftPlan->exist = checkIfsoExist(fftPlan->direction, fftPlan->precision, fftPlan->originalLength, fftPlan->hcfftlibtype);
  hcfftStatus status = hcfftBakePlanInternal(plHandle);
  fftPlan->filename = sfilename;
  fftPlan->kernellib = skernellib;
  return status;
}

hcfftStatus FFTPlan::hcfftBakePlanInternal(hcfftPlanHandle plHandle) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock);
  scopedLock sLock( *planLock, _T( "hcfftBakePlanInternal" ) );

  // if we have already baked the plan and nothing has changed since, we're done here
  if( fftPlan->baked == true ) {
    return HCFFT_SUCCEEDS;
  }

  //find product of lengths
  size_t maxLengthInAnyDim = 1;

  switch(fftPlan->dimension) {
    case HCFFT_3D:
      maxLengthInAnyDim = maxLengthInAnyDim > fftPlan->length[2] ? maxLengthInAnyDim : fftPlan->length[2];

    case HCFFT_2D:
      maxLengthInAnyDim = maxLengthInAnyDim > fftPlan->length[1] ? maxLengthInAnyDim : fftPlan->length[1];

    case HCFFT_1D:
      maxLengthInAnyDim = maxLengthInAnyDim > fftPlan->length[0] ? maxLengthInAnyDim : fftPlan->length[0];
  }

  bool rc = (fftPlan->ipLayout == HCFFT_REAL) || (fftPlan->opLayout == HCFFT_REAL);
  // upper bounds on transfrom lengths - address this in the next release
  size_t SP_MAX_LEN = 1 << 24;
  size_t DP_MAX_LEN = 1 << 22;

  if((fftPlan->precision == HCFFT_SINGLE) && (maxLengthInAnyDim > SP_MAX_LEN) && rc) {
    return HCFFT_INVALID;
  }

  if((fftPlan->precision == HCFFT_DOUBLE) && (maxLengthInAnyDim > DP_MAX_LEN) && rc) {
    return HCFFT_INVALID;
  }

  // release buffers, as these will be created only in EnqueueTransform
  if( NULL != fftPlan->twiddles ) {
    if( hc::am_free(fftPlan->twiddles) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    fftPlan->twiddles = NULL;
  }

  if( NULL != fftPlan->twiddleslarge ) {
    if( hc::am_free(fftPlan->twiddleslarge) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    fftPlan->twiddleslarge = NULL;
  }

  if( NULL != fftPlan->intBuffer ) {
    if( hc::am_free(fftPlan->intBuffer) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    fftPlan->intBuffer = NULL;
  }

  if( NULL != fftPlan->intBufferRC ) {
    if( hc::am_free(fftPlan->intBufferRC) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    fftPlan->intBufferRC = NULL;
  }

  if( NULL != fftPlan->intBufferC2R ) {
    if( hc::am_free(fftPlan->intBufferC2R) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    fftPlan->intBufferC2R = NULL;
  }

  if( fftPlan->userPlan ) { // confirm it is top-level plan (user plan)
    if(fftPlan->location == HCFFT_INPLACE) {
      if( (fftPlan->ipLayout == HCFFT_HERMITIAN_PLANAR) || (fftPlan->opLayout == HCFFT_HERMITIAN_PLANAR) ) {
        return HCFFT_INVALID;
      }
    }

    // Make sure strides & distance are same for C-C transforms
    if(fftPlan->location == HCFFT_INPLACE) {
      if( (fftPlan->ipLayout != HCFFT_REAL) && (fftPlan->opLayout != HCFFT_REAL) ) {
        // check strides
        for(size_t i = 0; i < fftPlan->dimension; i++)
          if(fftPlan->inStride[i] != fftPlan->outStride[i]) {
            return HCFFT_INVALID;
          }

        // check distance
        if(fftPlan->iDist != fftPlan->oDist) {
          return HCFFT_INVALID;
        }
      }
    }
  }

  if(fftPlan->gen == Copy) {
    fftPlan->GenerateKernel(plHandle, fftRepo, bakedPlanCount, fftPlan->exist);
    bakedPlanCount++;
    CompileKernels(plHandle, fftPlan->gen, fftPlan, fftPlan->plHandleOrigin, fftPlan->exist, fftPlan->originalLength, fftPlan->hcfftlibtype);
    fftPlan->baked = true;
    return HCFFT_SUCCEEDS;
  }

  // Compress the plan by discarding length '1' dimensions
  // decision to pick generator
  if( fftPlan->userPlan && !rc ) { // confirm it is top-level plan (user plan)
    size_t dmnsn = fftPlan->dimension;
    bool pow2flag = true;

    // switch case flows with no 'break' statements
    switch(fftPlan->dimension) {
      case HCFFT_3D:
        if(fftPlan->length[2] == 1) {
          dmnsn -= 1;
          fftPlan-> inStride.erase(fftPlan-> inStride.begin() + 2);
          fftPlan->outStride.erase(fftPlan->outStride.begin() + 2);
          fftPlan->   length.erase(fftPlan->   length.begin() + 2);
        } else {
          if( !IsPo2(fftPlan->length[2])) {
            pow2flag = false;
          }
        }

      case HCFFT_2D:
        if(fftPlan->length[1] == 1) {
          dmnsn -= 1;
          fftPlan-> inStride.erase(fftPlan-> inStride.begin() + 1);
          fftPlan->outStride.erase(fftPlan->outStride.begin() + 1);
          fftPlan->   length.erase(fftPlan->   length.begin() + 1);
        } else {
          if( !IsPo2(fftPlan->length[1])) {
            pow2flag = false;
          }
        }

      case HCFFT_1D:
        if( (fftPlan->length[0] == 1) && (dmnsn > 1) ) {
          dmnsn -= 1;
          fftPlan-> inStride.erase(fftPlan-> inStride.begin());
          fftPlan->outStride.erase(fftPlan->outStride.begin());
          fftPlan->   length.erase(fftPlan->   length.begin());
        } else {
          if( !IsPo2(fftPlan->length[0])) {
            pow2flag = false;
          }
        }
    }

//#TODO Check dimension value
    fftPlan->dimension = (hcfftDim)dmnsn;
  }

  // first time check transposed
  if (fftPlan->transposeType != HCFFT_NOTRANSPOSE && fftPlan->dimension != HCFFT_2D &&
      fftPlan->dimension == fftPlan->length.size()) {
    return HCFFT_ERROR;
  }

  // The largest vector we can transform in a single pass
  // depends on the GPU caps -- especially the amount of LDS
  // available
  //
  size_t Large1DThreshold = 0;
  fftPlan->GetMax1DLength(&Large1DThreshold);
  BUG_CHECK(Large1DThreshold > 1);

  //  Verify that the data passed to us is packed
  switch( fftPlan->dimension ) {
    case HCFFT_1D: {
        if ( !Is1DPossible(fftPlan->length[0], Large1DThreshold) ) {
          size_t hcLengths[] = { 1, 1, 0 };
          size_t in_1d, in_x, count;
          BUG_CHECK (IsPo2 (Large1DThreshold))

          if( IsPo2(fftPlan->length[0]) ) {
            // Enable block compute under these conditions
            if( (fftPlan->inStride[0] == 1) && (fftPlan->outStride[0] == 1) && !rc
                && (fftPlan->length[0] <= 262144 / width(fftPlan->precision)) && (fftPlan->length.size() <= 1)
                && (1 || (fftPlan->location == HCFFT_OUTOFPLACE))) {
              fftPlan->blockCompute = true;

              if(1 == width(fftPlan->precision)) {
                switch(fftPlan->length[0]) {
                  case 8192:
                    hcLengths[1] = 64;
                    break;

                  case 16384:
                    hcLengths[1] = 64;
                    break;

                  case 32768:
                    hcLengths[1] = 128;
                    break;

                  case 65536:
                    hcLengths[1] = 256;
                    break;

                  case 131072:
                    hcLengths[1] = 64;
                    break;

                  case 262144:
                    hcLengths[1] = 64;
                    break;

                  case 524288:
                    hcLengths[1] = 256;
                    break;

                  case 1048576:
                    hcLengths[1] = 256;
                    break;

                  default:
                    assert(false);
                }
              } else {
                switch(fftPlan->length[0]) {
                  case 4096:
                    hcLengths[1] = 64;
                    break;

                  case 8192:
                    hcLengths[1] = 64;
                    break;

                  case 16384:
                    hcLengths[1] = 64;
                    break;

                  case 32768:
                    hcLengths[1] = 128;
                    break;

                  case 65536:
                    hcLengths[1] = 64;
                    break;

                  case 131072:
                    hcLengths[1] = 64;
                    break;

                  case 262144:
                    hcLengths[1] = 128;
                    break;

                  case 524288:
                    hcLengths[1] = 256;
                    break;

                  default:
                    assert(false);
                }
              }
            } else {
              if(fftPlan->length[0] > (Large1DThreshold * Large1DThreshold) ) {
                hcLengths[1] = fftPlan->length[0] / Large1DThreshold;
              } else {
                in_1d = BitScanF (Large1DThreshold);  // this is log2(LARGE1D_THRESHOLD)
                in_x  = BitScanF (fftPlan->length[0]);  // this is log2(length)
                BUG_CHECK (in_1d > 0)
                count = in_x / in_1d;

                if (count * in_1d < in_x) {
                  count++;
                  in_1d = in_x / count;

                  if (in_1d * count < in_x) {
                    in_1d++;
                  }
                }

                hcLengths[1] = (size_t)1 << in_1d;
              }
            }
          } else {
            // This array must be kept sorted in the ascending order
            size_t supported[] = {  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 21, 22, 24,
                                    25, 26, 27, 28, 30, 32, 33, 35, 36, 39, 40, 42, 44, 45, 48, 49, 50, 52, 54,
                                    55, 56, 60, 63, 64, 65, 66, 70, 72, 75, 77, 78, 80, 81, 84, 88, 90, 91, 96,
                                    98, 99, 100, 104, 105, 108, 110, 112, 117, 120, 121, 125, 126, 128, 130, 132,
                                    135, 140, 143, 144, 147, 150, 154, 156, 160, 162, 165, 168, 169, 175, 176,
                                    180, 182, 189, 192, 195, 196, 198, 200, 208, 210, 216, 220, 224, 225, 231,
                                    234, 240, 242, 243, 245, 250, 252, 256, 260, 264, 270, 273, 275, 280, 286,
                                    288, 294, 297, 300, 308, 312, 315, 320, 324, 325, 330, 336, 338, 343, 350,
                                    351, 352, 360, 363, 364, 375, 378, 384, 385, 390, 392, 396, 400, 405, 416,
                                    420, 429, 432, 440, 441, 448, 450, 455, 462, 468, 480, 484, 486, 490, 495,
                                    500, 504, 507, 512, 520, 525, 528, 539, 540, 546, 550, 560, 567, 572, 576,
                                    585, 588, 594, 600, 605, 616, 624, 625, 630, 637, 640, 648, 650, 660, 672,
                                    675, 676, 686, 693, 700, 702, 704, 715, 720, 726, 728, 729, 735, 750, 756,
                                    768, 770, 780, 784, 792, 800, 810, 819, 825, 832, 840, 845, 847, 858, 864,
                                    875, 880, 882, 891, 896, 900, 910, 924, 936, 945, 960, 968, 972, 975, 980,
                                    990, 1000, 1001, 1008, 1014, 1024, 1029, 1040, 1050, 1053, 1056, 1078, 1080,
                                    1089, 1092, 1100, 1120, 1125, 1134, 1144, 1152, 1155, 1170, 1176, 1183, 1188,
                                    1200, 1210, 1215, 1225, 1232, 1248, 1250, 1260, 1274, 1280, 1287, 1296, 1300,
                                    1320, 1323, 1331, 1344, 1350, 1352, 1365, 1372, 1375, 1386, 1400, 1404, 1408,
                                    1430, 1440, 1452, 1456, 1458, 1470, 1485, 1500, 1512, 1521, 1536, 1540, 1560,
                                    1568, 1573, 1575, 1584, 1600, 1617, 1620, 1625, 1638, 1650, 1664, 1680, 1690,
                                    1694, 1701, 1715, 1716, 1728, 1750, 1755, 1760, 1764, 1782, 1792, 1800, 1815,
                                    1820, 1848, 1859, 1872, 1875, 1890, 1911, 1920, 1925, 1936, 1944, 1950, 1960,
                                    1980, 2000, 2002, 2016, 2025, 2028, 2048, 2058, 2079, 2080, 2100, 2106, 2112,
                                    2145, 2156, 2160, 2178, 2184, 2187, 2197, 2200, 2205, 2240, 2250, 2268, 2275,
                                    2288, 2304, 2310, 2340, 2352, 2366, 2376, 2400, 2401, 2420, 2430, 2450, 2457,
                                    2464, 2475, 2496, 2500, 2520, 2535, 2541, 2548, 2560, 2574, 2592, 2600, 2625,
                                    2640, 2646, 2662, 2673, 2688, 2695, 2700, 2704, 2730, 2744, 2750, 2772, 2800,
                                    2808, 2816, 2835, 2860, 2880, 2904, 2912, 2916, 2925, 2940, 2970, 3000, 3003,
                                    3024, 3025, 3042, 3072, 3080, 3087, 3120, 3125, 3136, 3146, 3150, 3159, 3168,
                                    3185, 3200, 3234, 3240, 3250, 3267, 3276, 3300, 3328, 3360, 3375, 3380, 3388,
                                    3402, 3430, 3432, 3456, 3465, 3500, 3510, 3520, 3528, 3549, 3564, 3575, 3584,
                                    3600, 3630, 3640, 3645, 3675, 3696, 3718, 3744, 3750, 3773, 3780, 3822, 3840,
                                    3850, 3861, 3872, 3888, 3900, 3920, 3960, 3969, 3993, 4000, 4004, 4032, 4050,
                                    4056, 4095, 4096
                                 };
            size_t lenSupported = sizeof(supported) / sizeof(supported[0]);
            size_t maxFactoredLength = (supported[lenSupported - 1] < Large1DThreshold) ? supported[lenSupported - 1] : Large1DThreshold;
            size_t halfPowerLength = (size_t)1 << ( (CeilPo2(fftPlan->length[0]) + 1) / 2 );
            size_t factoredLengthStart =  (halfPowerLength < maxFactoredLength) ? halfPowerLength : maxFactoredLength;
            size_t indexStart = 0;

            while(supported[indexStart] < factoredLengthStart) {
              indexStart++;
            }

            for(size_t i = indexStart; i >= 1; i--) {
              if( fftPlan->length[0] % supported[i] == 0 ) {
                if (Is1DPossible(supported[i], Large1DThreshold)) {
                  hcLengths[1] = supported[i];
                  break;
                }
              }
            }
          }

          size_t threshold = 4096;

          if (fftPlan->precision == HCFFT_DOUBLE) {
            threshold = 2048;
          }

          hcLengths[0] = fftPlan->length[0] / hcLengths[1];

          // Start of block where transposes are generated; 1D FFT
          while (1 && (fftPlan->ipLayout != HCFFT_REAL) && (fftPlan->opLayout != HCFFT_REAL)) {
            //if (!IsPo2(fftPlan->length[0])) break;
            if (fftPlan->length[0] <= Large1DThreshold) {
              break;
            }

            if (fftPlan->inStride[0] != 1 || fftPlan->outStride[0] != 1) {
              break;
            }

            if ( IsPo2(fftPlan->length[0]) &&
                 (fftPlan->length[0] <= 262144 / width(fftPlan->precision)) &&
                 ((fftPlan->location == HCFFT_OUTOFPLACE) || 1)) {
              break;
            }

            if ( hcLengths[0] <= 32 && hcLengths[1] <= 32) {
              break;
            }

            size_t biggerDim = hcLengths[0] > hcLengths[1] ? hcLengths[0] : hcLengths[1];
            size_t smallerDim = biggerDim == hcLengths[0] ? hcLengths[1] : hcLengths[0];
            size_t padding = 0;

            if( (smallerDim % 64 == 0) || (biggerDim % 64 == 0) ) {
              padding = 64;
            }

            hcfftGenerators transGen = Transpose_GCN;
            size_t dim_ratio = biggerDim / smallerDim;
            size_t dim_residue = biggerDim % smallerDim;
            //    If this is an in-place transform the
            //    input and output layout, dimensions and strides
            //    *MUST* be the same.
            //
            bool inStrideEqualsOutStride = true;

            for (size_t u = fftPlan->inStride.size(); u-- > 0; ) {
              if (fftPlan->inStride[u] != fftPlan->outStride[u]) {
                inStrideEqualsOutStride = false;
                break;
              }
            }

            //packed data is required for inplace transpose
            bool isDataPacked = true;

            for (size_t u = 0; u < fftPlan->inStride.size(); u++) {
              if (u == 0) {
                if (fftPlan->inStride[0] == 1) {
                  continue;
                } else {
                  isDataPacked = false;
                  break;
                }
              } else {
                size_t packDataSize = 1;

                for (size_t i = 0; i < u; i++) {
                  packDataSize *= fftPlan->length[i];
                }

                if (fftPlan->inStride[u] == packDataSize) {
                  continue;
                } else {
                  isDataPacked = false;
                  break;
                }
              }
            }

            if ( (fftPlan->tmpBufSize == 0 ) && !fftPlan->allOpsInplace) {
              fftPlan->tmpBufSize = (smallerDim + padding) * biggerDim *
                                    fftPlan->batchSize * fftPlan->ElementSize();

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                fftPlan->tmpBufSize *= fftPlan->length[index];
              }
            }

            //Transpose
            //Input --> tmp buffer
            hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, hcLengths );
            FFTPlan* trans1Plan = NULL;
            lockRAII* trans1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTX, trans1Plan, trans1Lock );
            trans1Plan->location     = fftPlan->allOpsInplace ? HCFFT_INPLACE : HCFFT_OUTOFPLACE;
            trans1Plan->precision     = fftPlan->precision;
            trans1Plan->tmpBufSize    = 0;
            trans1Plan->batchSize     = fftPlan->batchSize;
            trans1Plan->envelope    = fftPlan->envelope;
            trans1Plan->ipLayout   = fftPlan->ipLayout;
            trans1Plan->opLayout  = fftPlan->allOpsInplace ? fftPlan->ipLayout  : HCFFT_COMPLEX_INTERLEAVED;
            trans1Plan->inStride[0]   = fftPlan->inStride[0];
            trans1Plan->inStride[1]   = hcLengths[0];
            trans1Plan->outStride[0]  = 1;
            trans1Plan->outStride[1]  = hcLengths[1] + padding;
            trans1Plan->iDist         = fftPlan->iDist;
            trans1Plan->oDist         = hcLengths[0] * trans1Plan->outStride[1];
            trans1Plan->gen           = transGen;
            trans1Plan->transflag     = true;
            trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans1Plan->originalLength  = fftPlan->originalLength;
            trans1Plan->acc  = fftPlan->acc;
            trans1Plan->exist  = fftPlan->exist;
            trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

            if (trans1Plan->gen == Transpose_NONSQUARE || trans1Plan->gen == Transpose_SQUARE) {
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                //trans1Plan->length.push_back(fftPlan->length[index]);
                /*
                replacing the line above with the two lines below since:
                fftPlan is still 1D, thus the broken down transpose should be 2D not 3D
                the batchSize for the transpose should increase accordingly.
                the iDist should decrease accordingly. Push back to length will cause a 3D transpose
                */
                trans1Plan->batchSize = trans1Plan->batchSize * fftPlan->length[index];
                trans1Plan->iDist = trans1Plan->iDist / fftPlan->length[index];
                trans1Plan->inStride.push_back(fftPlan->inStride[index]);
                trans1Plan->outStride.push_back(trans1Plan->oDist);
                trans1Plan->oDist *= fftPlan->length[index];
              }
            } else {
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                trans1Plan->length.push_back(fftPlan->length[index]);
                trans1Plan->inStride.push_back(fftPlan->inStride[index]);
                trans1Plan->outStride.push_back(trans1Plan->oDist);
                trans1Plan->oDist *= fftPlan->length[index];
              }
            }

            hcfftBakePlanInternal(fftPlan->planTX);
            //Row transform
            //tmp->output
            //size hcLengths[1], batch hcLengths[0], with length[0] twiddle factor multiplication
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &hcLengths[1] );
            FFTPlan* row1Plan = NULL;
            lockRAII* row1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planX, row1Plan, row1Lock );
            row1Plan->location     = fftPlan->allOpsInplace ? HCFFT_INPLACE : HCFFT_OUTOFPLACE;
            row1Plan->precision     = fftPlan->precision;
            row1Plan->forwardScale  = 1.0f;
            row1Plan->backwardScale = 1.0f;
            row1Plan->tmpBufSize    = 0;
            row1Plan->batchSize     = fftPlan->batchSize;
            row1Plan->gen     = fftPlan->gen;
            row1Plan->envelope    = fftPlan->envelope;
            // twiddling is done in row2
            row1Plan->large1D   = 0;
            row1Plan->length.push_back(hcLengths[0]);
            row1Plan->ipLayout   = fftPlan->allOpsInplace ? fftPlan->ipLayout : HCFFT_COMPLEX_INTERLEAVED;
            row1Plan->opLayout  = fftPlan->opLayout;
            row1Plan->inStride[0]   = 1;
            row1Plan->outStride[0]  = fftPlan->outStride[0];
            row1Plan->inStride.push_back(hcLengths[1] + padding);
            row1Plan->outStride.push_back(hcLengths[1]);
            row1Plan->iDist         = hcLengths[0] * row1Plan->inStride[1];
            row1Plan->oDist         = fftPlan->oDist;
            row1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            row1Plan->originalLength  = fftPlan->originalLength;
            row1Plan->acc  = fftPlan->acc;
            row1Plan->exist  = fftPlan->exist;
            row1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

            for (size_t index = 1; index < fftPlan->length.size(); index++) {
              row1Plan->length.push_back(fftPlan->length[index]);
              row1Plan->inStride.push_back(row1Plan->iDist);
              row1Plan->iDist *= fftPlan->length[index];
              row1Plan->outStride.push_back(fftPlan->outStride[index]);
            }

            hcfftBakePlanInternal(fftPlan->planX);
            //Transpose 2
            //Output --> tmp buffer
            hcLengths[2] = hcLengths[0];
            hcfftCreateDefaultPlanInternal( &fftPlan->planTY, HCFFT_2D, &hcLengths[1] );
            FFTPlan* trans2Plan = NULL;
            lockRAII* trans2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTY, trans2Plan, trans2Lock );
            trans2Plan->location     = fftPlan->allOpsInplace ? HCFFT_INPLACE : HCFFT_OUTOFPLACE;
            trans2Plan->precision     = fftPlan->precision;
            trans2Plan->tmpBufSize    = 0;
            trans2Plan->batchSize     = fftPlan->batchSize;
            trans2Plan->envelope    = fftPlan->envelope;
            trans2Plan->ipLayout   = fftPlan->opLayout;
            trans2Plan->opLayout  = fftPlan->allOpsInplace ? fftPlan->ipLayout : HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->inStride[0]   = fftPlan->outStride[0];
            trans2Plan->inStride[1]   = hcLengths[1];
            trans2Plan->outStride[0]  = 1;
            trans2Plan->outStride[1]  = hcLengths[0] + padding;
            trans2Plan->iDist         = fftPlan->oDist;
            trans2Plan->oDist         = hcLengths[1] * trans2Plan->outStride[1];
            trans2Plan->gen           = transGen;
            trans2Plan->large1D   = fftPlan->length[0];
            trans2Plan->transflag     = true;
            trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans2Plan->originalLength  = fftPlan->originalLength;
            trans2Plan->acc  = fftPlan->acc;
            trans2Plan->exist  = fftPlan->exist;
            trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

            if (trans2Plan->gen == Transpose_NONSQUARE || trans2Plan->gen == Transpose_SQUARE) { // inplace transpose
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                //trans2Plan->length.push_back(fftPlan->length[index]);
                /*
                replacing the line above with the two lines below since:
                fftPlan is still 1D, thus the broken down transpose should be 2D not 3D
                the batchSize for the transpose should increase accordingly.
                the iDist should decrease accordingly. Push back to length will cause a 3D transpose
                */
                trans2Plan->batchSize = trans2Plan->batchSize * fftPlan->length[index];
                trans2Plan->iDist = trans2Plan->iDist / fftPlan->length[index];
                trans2Plan->inStride.push_back(fftPlan->outStride[index]);
                trans2Plan->outStride.push_back(trans2Plan->oDist);
                trans2Plan->oDist *= fftPlan->length[index];
              }
            } else {
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                trans2Plan->length.push_back(fftPlan->length[index]);
                trans2Plan->inStride.push_back(fftPlan->outStride[index]);
                trans2Plan->outStride.push_back(trans2Plan->oDist);
                trans2Plan->oDist *= fftPlan->length[index];
              }
            }

            hcfftBakePlanInternal(fftPlan->planTY);
            //Row transform 2
            //tmp->tmp
            //size hcLengths[0], batch hcLengths[1]
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &hcLengths[0] );
            FFTPlan* row2Plan = NULL;
            lockRAII* row2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planY, row2Plan, row2Lock );
            row2Plan->location     = HCFFT_INPLACE;
            row2Plan->precision     = fftPlan->precision;
            row2Plan->forwardScale  = fftPlan->forwardScale;
            row2Plan->backwardScale = fftPlan->backwardScale;
            row2Plan->tmpBufSize    = 0;
            row2Plan->batchSize     = fftPlan->batchSize;
            row2Plan->gen     = fftPlan->gen;
            row2Plan->envelope    = fftPlan->envelope;
            row2Plan->length.push_back(hcLengths[1]);
            row2Plan->ipLayout   = fftPlan->allOpsInplace ? fftPlan->ipLayout : HCFFT_COMPLEX_INTERLEAVED;
            row2Plan->opLayout  = fftPlan->allOpsInplace ? fftPlan->ipLayout : HCFFT_COMPLEX_INTERLEAVED;
            row2Plan->inStride[0]   = 1;
            row2Plan->outStride[0]  = 1;
            row2Plan->inStride.push_back(hcLengths[0] + padding);
            row2Plan->outStride.push_back(hcLengths[0] + padding);
            row2Plan->iDist         = hcLengths[1] * row2Plan->inStride[1];
            row2Plan->oDist         = hcLengths[1] * row2Plan->outStride[1];
            row2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            row2Plan->originalLength  = fftPlan->originalLength;
            row2Plan->acc  = fftPlan->acc;
            row2Plan->exist  = fftPlan->exist;
            row2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

            for (size_t index = 1; index < fftPlan->length.size(); index++) {
              row2Plan->length.push_back(fftPlan->length[index]);
              row2Plan->inStride.push_back(row2Plan->iDist);
              row2Plan->outStride.push_back(row2Plan->oDist);
              row2Plan->iDist *= fftPlan->length[index];
              row2Plan->oDist *= fftPlan->length[index];
            }

            hcfftBakePlanInternal(fftPlan->planY);
            //Transpose 3
            //tmp --> output
            hcfftCreateDefaultPlanInternal( &fftPlan->planTZ, HCFFT_2D, hcLengths );
            FFTPlan* trans3Plan = NULL;
            lockRAII* trans3Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTZ, trans3Plan, trans3Lock);
            trans3Plan->location     = fftPlan->allOpsInplace ? HCFFT_INPLACE : HCFFT_OUTOFPLACE;
            trans3Plan->precision     = fftPlan->precision;
            trans3Plan->tmpBufSize    = 0;
            trans3Plan->batchSize     = fftPlan->batchSize;
            trans3Plan->envelope    = fftPlan->envelope;
            trans3Plan->ipLayout   = fftPlan->allOpsInplace ? fftPlan->ipLayout : HCFFT_COMPLEX_INTERLEAVED;
            trans3Plan->opLayout  = fftPlan->opLayout;
            trans3Plan->inStride[0]   = 1;
            trans3Plan->inStride[1]   = hcLengths[0] + padding;
            trans3Plan->outStride[0]  = fftPlan->outStride[0];
            trans3Plan->outStride[1]  = hcLengths[1];
            trans3Plan->iDist         = hcLengths[1] * trans3Plan->inStride[1];
            trans3Plan->oDist         = fftPlan->oDist;
            trans3Plan->gen           = transGen;
            trans3Plan->transflag     = true;
            trans3Plan->transOutHorizontal = true;
            trans3Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans3Plan->originalLength  = fftPlan->originalLength;
            trans3Plan->acc  = fftPlan->acc;
            trans3Plan->exist  = fftPlan->exist;
            trans3Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

            if (trans3Plan->gen == Transpose_NONSQUARE) { // inplace transpose
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                //trans3Plan->length.push_back(fftPlan->length[index]);
                /*
                replacing the line above with the two lines below since:
                fftPlan is still 1D, thus the broken down transpose should be 2D not 3D
                the batchSize for the transpose should increase accordingly.
                the iDist should decrease accordingly. Push back to length will cause a 3D transpose
                */
                trans3Plan->batchSize = trans3Plan->batchSize * fftPlan->length[index];
                //trans3Plan->iDist = trans3Plan->iDist / fftPlan->length[index];
                //trans3Plan->inStride.push_back(trans3Plan->iDist);
                trans3Plan->inStride.push_back(fftPlan->inStride[index]);
                //trans3Plan->iDist *= fftPlan->length[index];
                trans3Plan->outStride.push_back(fftPlan->outStride[index]);
              }
            } else if (trans3Plan->gen == Transpose_SQUARE) {
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                trans3Plan->batchSize = trans3Plan->batchSize * fftPlan->length[index];
                //trans3Plan->iDist = trans3Plan->iDist / fftPlan->length[index];
                //trans3Plan->inStride.push_back(trans3Plan->iDist);
                trans3Plan->inStride.push_back(fftPlan->inStride[index]);
                //trans3Plan->iDist *= fftPlan->length[index];
                trans3Plan->outStride.push_back(fftPlan->outStride[index]);
              }
            } else {
              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                trans3Plan->length.push_back(fftPlan->length[index]);
                trans3Plan->inStride.push_back(trans3Plan->iDist);
                trans3Plan->iDist *= fftPlan->length[index];
                trans3Plan->outStride.push_back(fftPlan->outStride[index]);
              }
            }

            hcfftBakePlanInternal(fftPlan->planTZ);
            fftPlan->transflag = true;
            fftPlan->baked = true;
            return  HCFFT_SUCCEEDS;
          }

          size_t length0 = hcLengths[0];
          size_t length1 = hcLengths[1];

          // For real transforms
          // Special case optimization with 5-step algorithm
          if( (fftPlan->ipLayout == HCFFT_REAL) && IsPo2(fftPlan->length[0])
              && (fftPlan->length.size() == 1)
              && (fftPlan->inStride[0] == 1) && (fftPlan->outStride[0] == 1)
              && (fftPlan->length[0] > 4096) && (fftPlan->length.size() == 1) ) {
            ARG_CHECK(hcLengths[0] <= Large1DThreshold);
            size_t biggerDim = hcLengths[0] > hcLengths[1] ? hcLengths[0] : hcLengths[1];
            size_t smallerDim = biggerDim == hcLengths[0] ? hcLengths[1] : hcLengths[0];
            size_t padding = 0;

            if( (smallerDim % 64 == 0) || (biggerDim % 64 == 0) ) {
              padding = 64;
            }

            if (fftPlan->tmpBufSize == 0 ) {
              size_t Nf = (1 + smallerDim / 2) * biggerDim;
              fftPlan->tmpBufSize = (smallerDim + padding) * biggerDim / 2;

              if(fftPlan->tmpBufSize < Nf) {
                fftPlan->tmpBufSize = Nf;
              }

              fftPlan->tmpBufSize *= ( fftPlan->batchSize * fftPlan->ElementSize() );

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                fftPlan->tmpBufSize *= fftPlan->length[index];
              }
            }

            if (fftPlan->tmpBufSizeRC == 0 ) {
              fftPlan->tmpBufSizeRC = fftPlan->tmpBufSize;
            }

            //Transpose
            //Input --> tmp buffer
            hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, hcLengths );
            FFTPlan* trans1Plan = NULL;
            lockRAII* trans1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTX, trans1Plan, trans1Lock );
            trans1Plan->location     = HCFFT_OUTOFPLACE;
            trans1Plan->precision     = fftPlan->precision;
            trans1Plan->tmpBufSize    = 0;
            trans1Plan->batchSize     = fftPlan->batchSize;
            trans1Plan->envelope    = fftPlan->envelope;
            trans1Plan->ipLayout   = fftPlan->ipLayout;
            trans1Plan->opLayout  = HCFFT_REAL;
            trans1Plan->inStride[0]   = fftPlan->inStride[0];
            trans1Plan->inStride[1]   = hcLengths[0];
            trans1Plan->outStride[0]  = 1;
            trans1Plan->outStride[1]  = hcLengths[1] + padding;
            trans1Plan->iDist         = fftPlan->iDist;
            trans1Plan->oDist         = hcLengths[0] * trans1Plan->outStride[1];
            trans1Plan->gen           = Transpose_GCN;
            trans1Plan->transflag     = true;
            trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans1Plan->originalLength  = fftPlan->originalLength;
            trans1Plan->acc  = fftPlan->acc;
            trans1Plan->exist  = fftPlan->exist;
            trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planTX);
            //Row transform
            //tmp->output
            //size hcLengths[1], batch hcLengths[0], with length[0] twiddle factor multiplication
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &hcLengths[1] );
            FFTPlan* row1Plan = NULL;
            lockRAII* row1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planX, row1Plan, row1Lock );
            row1Plan->location     = HCFFT_OUTOFPLACE;
            row1Plan->precision     = fftPlan->precision;
            row1Plan->forwardScale  = 1.0f;
            row1Plan->backwardScale = 1.0f;
            row1Plan->tmpBufSize    = 0;
            row1Plan->batchSize     = fftPlan->batchSize;
            row1Plan->gen     = fftPlan->gen;
            row1Plan->envelope    = fftPlan->envelope;
            // twiddling is done in row2
            row1Plan->large1D   = 0;
            row1Plan->length.push_back(hcLengths[0]);
            row1Plan->ipLayout   = HCFFT_REAL;
            row1Plan->opLayout  = HCFFT_HERMITIAN_INTERLEAVED;
            row1Plan->inStride[0]   = 1;
            row1Plan->outStride[0]  = 1;
            row1Plan->inStride.push_back(hcLengths[1] + padding);
            row1Plan->outStride.push_back(1 + hcLengths[1] / 2);
            row1Plan->iDist         = hcLengths[0] * row1Plan->inStride[1];
            row1Plan->oDist         = hcLengths[0] * row1Plan->outStride[1];
            row1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            row1Plan->originalLength  = fftPlan->originalLength;
            row1Plan->acc  = fftPlan->acc;
            row1Plan->exist  = fftPlan->exist;
            row1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planX);
            //Transpose 2
            //Output --> tmp buffer
            hcLengths[2] = hcLengths[0];
            hcfftCreateDefaultPlanInternal( &fftPlan->planTY, HCFFT_2D, &hcLengths[1] );
            FFTPlan* trans2Plan = NULL;
            lockRAII* trans2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTY, trans2Plan, trans2Lock );
            trans2Plan->transflag = true;
            size_t transLengths[2];
            transLengths[0] = 1 + hcLengths[1] / 2;
            transLengths[1] = hcLengths[0];
            hcfftSetPlanLength( fftPlan->planTY, HCFFT_2D, transLengths );
            trans2Plan->location     = HCFFT_OUTOFPLACE;
            trans2Plan->precision     = fftPlan->precision;
            trans2Plan->tmpBufSize    = 0;
            trans2Plan->batchSize     = fftPlan->batchSize;
            trans2Plan->envelope    = fftPlan->envelope;
            trans2Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->inStride[0]   = 1;
            trans2Plan->inStride[1]   = 1 + hcLengths[1] / 2;
            trans2Plan->outStride[0]  = 1;
            trans2Plan->outStride[1]  = hcLengths[0];
            trans2Plan->iDist         = hcLengths[0] * trans2Plan->inStride[1];
            trans2Plan->oDist         = (1 + hcLengths[1] / 2) * trans2Plan->outStride[1];
            trans2Plan->gen           = Transpose_GCN;
            trans2Plan->transflag     = true;
            trans2Plan->transOutHorizontal = true;
            trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans2Plan->originalLength  = fftPlan->originalLength;
            trans2Plan->acc  = fftPlan->acc;
            trans2Plan->exist  = fftPlan->exist;
            trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planTY);
            //Row transform 2
            //tmp->tmp
            //size hcLengths[0], batch hcLengths[1]
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &hcLengths[0] );
            FFTPlan* row2Plan = NULL;
            lockRAII* row2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planY, row2Plan, row2Lock );
            row2Plan->location     = HCFFT_OUTOFPLACE;
            row2Plan->precision     = fftPlan->precision;
            row2Plan->forwardScale  = fftPlan->forwardScale;
            row2Plan->backwardScale = fftPlan->backwardScale;
            row2Plan->tmpBufSize    = 0;
            row2Plan->batchSize     = fftPlan->batchSize;
            row2Plan->gen     = fftPlan->gen;
            row2Plan->envelope    = fftPlan->envelope;
            row2Plan->length.push_back(1 + hcLengths[1] / 2);
            row2Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
            row2Plan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            row2Plan->inStride[0]   = 1;
            row2Plan->outStride[0]  = 1;
            row2Plan->inStride.push_back(hcLengths[0]);
            row2Plan->outStride.push_back(1 + hcLengths[0] / 2);
            row2Plan->iDist         = (1 + hcLengths[1] / 2) * row2Plan->inStride[1];
            row2Plan->oDist         = hcLengths[1] * row2Plan->outStride[1];
            row2Plan->large1D   = fftPlan->length[0];
            row2Plan->twiddleFront  = true;
            row2Plan->realSpecial = true;
            row2Plan->realSpecial_Nr = hcLengths[1];
            row2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            row2Plan->originalLength  = fftPlan->originalLength;
            row2Plan->acc  = fftPlan->acc;
            row2Plan->exist  = fftPlan->exist;
            row2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planY);
            //Transpose 3
            //tmp --> output
            hcfftCreateDefaultPlanInternal( &fftPlan->planTZ, HCFFT_2D, hcLengths );
            FFTPlan* trans3Plan = NULL;
            lockRAII* trans3Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTZ, trans3Plan, trans3Lock );
            trans3Plan->transflag = true;
            transLengths[0] = 1 + hcLengths[0] / 2;
            transLengths[1] = hcLengths[1];
            hcfftSetPlanLength( fftPlan->planTZ, HCFFT_2D, transLengths );
            trans3Plan->location     = HCFFT_OUTOFPLACE;
            trans3Plan->precision     = fftPlan->precision;
            trans3Plan->tmpBufSize    = 0;
            trans3Plan->batchSize     = fftPlan->batchSize;
            trans3Plan->envelope    = fftPlan->envelope;
            trans3Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;

            if(fftPlan->opLayout == HCFFT_HERMITIAN_PLANAR) {
              trans3Plan->opLayout  = HCFFT_COMPLEX_PLANAR;
            } else {
              trans3Plan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            }

            trans3Plan->inStride[0]   = 1;
            trans3Plan->inStride[1]   = 1 + hcLengths[0] / 2;
            trans3Plan->outStride[0]  = 1;
            trans3Plan->outStride[1]  = hcLengths[1];
            trans3Plan->iDist         = hcLengths[1] * trans3Plan->inStride[1];
            trans3Plan->oDist         = fftPlan->oDist;
            trans3Plan->gen           = Transpose_GCN;
            trans3Plan->transflag     = true;
            trans3Plan->realSpecial   = true;
            trans3Plan->transOutHorizontal = true;
            trans3Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans3Plan->originalLength  = fftPlan->originalLength;
            trans3Plan->acc  = fftPlan->acc;
            trans3Plan->exist  = fftPlan->exist;
            trans3Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planTZ);
            fftPlan->transflag = true;
            fftPlan->baked = true;
            return  HCFFT_SUCCEEDS;
          } else if(fftPlan->ipLayout == HCFFT_REAL) {
            if (fftPlan->tmpBufSizeRC == 0 ) {
              fftPlan->tmpBufSizeRC = length0 * length1 *
                                      fftPlan->batchSize * fftPlan->ElementSize();

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                fftPlan->tmpBufSizeRC *= fftPlan->length[index];
              }
            }

            // column FFT, size hcLengths[1], batch hcLengths[0], with length[0] twiddle factor multiplication
            // transposed output
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &hcLengths[1] );
            FFTPlan* colTPlan = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planX, colTPlan, colLock );
            // current plan is to create intermediate buffer, packed and interleave
            // This is a column FFT, the first elements distance between each FFT is the distance of the first two
            // elements in the original buffer. Like a transpose of the matrix
            // we need to pass hcLengths[0] and instride size to kernel, so kernel can tell the difference
            //this part are common for both passes
            colTPlan->location     = HCFFT_OUTOFPLACE;
            colTPlan->precision     = fftPlan->precision;
            colTPlan->forwardScale  = 1.0f;
            colTPlan->backwardScale = 1.0f;
            colTPlan->tmpBufSize    = 0;
            colTPlan->batchSize     = fftPlan->batchSize;
            colTPlan->gen     = fftPlan->gen;
            colTPlan->envelope      = fftPlan->envelope;
            //Pass large1D flag to confirm we need multiply twiddle factor
            colTPlan->large1D       = fftPlan->length[0];
            colTPlan->RCsimple    = true;
            colTPlan->length.push_back(hcLengths[0]);
            // first Pass
            colTPlan->ipLayout   = fftPlan->ipLayout;
            colTPlan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            colTPlan->inStride[0]   = fftPlan->inStride[0] * hcLengths[0];
            colTPlan->outStride[0]  = 1;
            colTPlan->iDist         = fftPlan->iDist;
            colTPlan->oDist         = length0 * length1;//fftPlan->length[0];
            colTPlan->inStride.push_back(fftPlan->inStride[0]);
            colTPlan->outStride.push_back(length1);//hcLengths[1]);

            for (size_t index = 1; index < fftPlan->length.size(); index++) {
              colTPlan->length.push_back(fftPlan->length[index]);
              colTPlan->inStride.push_back(fftPlan->inStride[index]);
              // tmp buffer is tightly packed
              colTPlan->outStride.push_back(colTPlan->oDist);
              colTPlan->oDist        *= fftPlan->length[index];
            }

            colTPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colTPlan->originalLength  = fftPlan->originalLength;
            colTPlan->acc  = fftPlan->acc;
            colTPlan->exist  = fftPlan->exist;
            colTPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planX);
            //another column FFT, size hcLengths[0], batch hcLengths[1], output without transpose
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D,  &hcLengths[0] );
            FFTPlan* col2Plan = NULL;
            lockRAII* rowLock = NULL;
            fftRepo.getPlan( fftPlan->planY, col2Plan, rowLock );
            // This is second column fft, intermediate buffer is packed and interleaved
            // we need to pass hcLengths[1] and instride size to kernel, so kernel can tell the difference
            // common part for both passes
            col2Plan->location     = HCFFT_INPLACE;
            col2Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
            col2Plan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            col2Plan->precision     = fftPlan->precision;
            col2Plan->forwardScale  = fftPlan->forwardScale;
            col2Plan->backwardScale = fftPlan->backwardScale;
            col2Plan->tmpBufSize    = 0;
            col2Plan->batchSize     = fftPlan->batchSize;
            col2Plan->gen     = fftPlan->gen;
            col2Plan->envelope      = fftPlan->envelope;
            col2Plan->length.push_back(length1);
            col2Plan->inStride[0]  = length1;
            col2Plan->inStride.push_back(1);
            col2Plan->iDist        = length0 * length1;
            // make sure colTPlan (first column plan) does not recurse, otherwise large twiddle mul
            // cannot be done with this algorithm sequence
            assert(colTPlan->planX == 0);
            col2Plan->outStride[0] = length1;
            col2Plan->outStride.push_back(1);
            col2Plan->oDist         = length0 * length1;

            for (size_t index = 1; index < fftPlan->length.size(); index++) {
              col2Plan->length.push_back(fftPlan->length[index]);
              col2Plan->inStride.push_back(col2Plan->iDist);
              col2Plan->outStride.push_back(col2Plan->oDist);
              col2Plan->iDist   *= fftPlan->length[index];
              col2Plan->oDist   *= fftPlan->length[index];
            }

            col2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            col2Plan->originalLength  = fftPlan->originalLength;
            col2Plan->acc  = fftPlan->acc;
            col2Plan->exist  = fftPlan->exist;
            col2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planY);

            if ( (fftPlan->opLayout == HCFFT_HERMITIAN_INTERLEAVED) ||
                 (fftPlan->opLayout == HCFFT_HERMITIAN_PLANAR) ) {
              // copy plan to get back to hermitian
              hcfftCreateDefaultPlanInternal( &fftPlan->planRCcopy, HCFFT_1D,  &fftPlan->length[0]);
              FFTPlan* copyPlan = NULL;
              lockRAII* copyLock  = NULL;
              fftRepo.getPlan( fftPlan->planRCcopy, copyPlan, copyLock );
              // This is second column fft, intermediate buffer is packed and interleaved
              // we need to pass hcLengths[1] and instride size to kernel, so kernel can tell the difference
              // common part for both passes
              copyPlan->location     = HCFFT_OUTOFPLACE;
              copyPlan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
              copyPlan->opLayout  = fftPlan->opLayout;
              copyPlan->precision     = fftPlan->precision;
              copyPlan->forwardScale  = 1.0f;
              copyPlan->backwardScale = 1.0f;
              copyPlan->tmpBufSize    = 0;
              copyPlan->batchSize     = fftPlan->batchSize;
              copyPlan->gen     = Copy;
              copyPlan->envelope    = fftPlan->envelope;
              copyPlan->inStride[0]  = 1;
              copyPlan->iDist        = fftPlan->length[0];
              copyPlan->outStride[0] = fftPlan->outStride[0];
              copyPlan->oDist         = fftPlan->oDist;
              copyPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
              copyPlan->originalLength  = fftPlan->originalLength;

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                copyPlan->length.push_back(fftPlan->length[index]);
                copyPlan->inStride.push_back(copyPlan->inStride[index - 1] * fftPlan->length[index - 1]);
                copyPlan->iDist   *= fftPlan->length[index];
                copyPlan->outStride.push_back(fftPlan->outStride[index]);
              }

              copyPlan->acc  = fftPlan->acc;
              copyPlan->exist  = fftPlan->exist;
              copyPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planRCcopy);
            }
          } else if(fftPlan->opLayout == HCFFT_REAL) {
            if (fftPlan->tmpBufSizeRC == 0 ) {
              fftPlan->tmpBufSizeRC = length0 * length1 *
                                      fftPlan->batchSize * fftPlan->ElementSize();

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                fftPlan->tmpBufSizeRC *= fftPlan->length[index];
              }
            }

            if ((fftPlan->ipLayout == HCFFT_HERMITIAN_INTERLEAVED) ||
                (fftPlan->ipLayout == HCFFT_HERMITIAN_PLANAR)) {
              // copy plan to from hermitian to full complex
              hcfftCreateDefaultPlanInternal( &fftPlan->planRCcopy, HCFFT_1D,  &fftPlan->length[0] );
              FFTPlan* copyPlan = NULL;
              lockRAII* copyLock  = NULL;
              fftRepo.getPlan( fftPlan->planRCcopy, copyPlan, copyLock );
              // This is second column fft, intermediate buffer is packed and interleaved
              // we need to pass hcLengths[1] and instride size to kernel, so kernel can tell the difference
              // common part for both passes
              copyPlan->location     = HCFFT_OUTOFPLACE;
              copyPlan->ipLayout   = fftPlan->ipLayout;
              copyPlan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
              copyPlan->precision     = fftPlan->precision;
              copyPlan->forwardScale  = 1.0f;
              copyPlan->backwardScale = 1.0f;
              copyPlan->tmpBufSize    = 0;
              copyPlan->batchSize     = fftPlan->batchSize;
              copyPlan->gen     = Copy;
              copyPlan->envelope    = fftPlan->envelope;
              copyPlan->inStride[0]  = fftPlan->inStride[0];
              copyPlan->iDist        = fftPlan->iDist;
              copyPlan->outStride[0]  = 1;
              copyPlan->oDist        = fftPlan->length[0];

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                copyPlan->length.push_back(fftPlan->length[index]);
                copyPlan->outStride.push_back(copyPlan->outStride[index - 1] * fftPlan->length[index - 1]);
                copyPlan->oDist   *= fftPlan->length[index];
                copyPlan->inStride.push_back(fftPlan->inStride[index]);
              }

              copyPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
              copyPlan->originalLength  = fftPlan->originalLength;
              copyPlan->acc  = fftPlan->acc;
              copyPlan->exist  = fftPlan->exist;
              copyPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planRCcopy);
            }

            // column FFT, size hcLengths[1], batch hcLengths[0], with length[0] twiddle factor multiplication
            // transposed output
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &hcLengths[1] );
            FFTPlan* colTPlan = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planX, colTPlan, colLock );
            // current plan is to create intermediate buffer, packed and interleave
            // This is a column FFT, the first elements distance between each FFT is the distance of the first two
            // elements in the original buffer. Like a transpose of the matrix
            // we need to pass hcLengths[0] and instride size to kernel, so kernel can tell the difference
            //this part are common for both passes
            colTPlan->precision     = fftPlan->precision;
            colTPlan->forwardScale  = 1.0f;
            colTPlan->backwardScale = 1.0f;
            colTPlan->tmpBufSize    = 0;
            colTPlan->batchSize     = fftPlan->batchSize;
            colTPlan->gen     = fftPlan->gen;
            colTPlan->envelope      = fftPlan->envelope;
            //Pass large1D flag to confirm we need multiply twiddle factor
            colTPlan->large1D       = fftPlan->length[0];
            colTPlan->length.push_back(hcLengths[0]);
            // first Pass
            colTPlan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
            colTPlan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            colTPlan->inStride[0]  = length0;
            colTPlan->inStride.push_back(1);
            colTPlan->iDist        = length0 * length1;
            colTPlan->outStride[0] = length0;
            colTPlan->outStride.push_back(1);
            colTPlan->oDist         = length0 * length1;

            for (size_t index = 1; index < fftPlan->length.size(); index++) {
              colTPlan->length.push_back(fftPlan->length[index]);
              colTPlan->inStride.push_back(colTPlan->iDist);
              colTPlan->outStride.push_back(colTPlan->oDist);
              colTPlan->iDist   *= fftPlan->length[index];
              colTPlan->oDist   *= fftPlan->length[index];
            }

            if ((fftPlan->ipLayout == HCFFT_HERMITIAN_INTERLEAVED) ||
                (fftPlan->ipLayout == HCFFT_HERMITIAN_PLANAR)) {
              colTPlan->location = HCFFT_INPLACE;
            } else {
              colTPlan->location = HCFFT_OUTOFPLACE;
            }

            colTPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colTPlan->originalLength  = fftPlan->originalLength;
            colTPlan->acc  = fftPlan->acc;
            colTPlan->exist  = fftPlan->exist;
            colTPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planX);
            //another column FFT, size hcLengths[0], batch hcLengths[1], output without transpose
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D,  &hcLengths[0] );
            FFTPlan* col2Plan = NULL;
            lockRAII* rowLock = NULL;
            fftRepo.getPlan( fftPlan->planY, col2Plan, rowLock );
            // This is second column fft, intermediate buffer is packed and interleaved
            // we need to pass hcLengths[1] and instride size to kernel, so kernel can tell the difference
            // common part for both passes
            col2Plan->location     = HCFFT_OUTOFPLACE;
            col2Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
            col2Plan->opLayout  = fftPlan->opLayout;
            col2Plan->precision     = fftPlan->precision;
            col2Plan->forwardScale  = fftPlan->forwardScale;
            col2Plan->backwardScale = fftPlan->backwardScale;
            col2Plan->tmpBufSize    = 0;
            col2Plan->batchSize     = fftPlan->batchSize;
            col2Plan->gen     = fftPlan->gen;
            col2Plan->envelope      = fftPlan->envelope;
            col2Plan->RCsimple = true;
            col2Plan->length.push_back(length1);
            col2Plan->inStride[0]  = 1;
            col2Plan->inStride.push_back(length0);
            col2Plan->iDist        = length0 * length1;
            col2Plan->outStride[0] = length1 * fftPlan->outStride[0];
            col2Plan->outStride.push_back(fftPlan->outStride[0]);
            col2Plan->oDist         = fftPlan->oDist;

            for (size_t index = 1; index < fftPlan->length.size(); index++) {
              col2Plan->length.push_back(fftPlan->length[index]);
              col2Plan->inStride.push_back(col2Plan->iDist);
              col2Plan->iDist   *= fftPlan->length[index];
              col2Plan->outStride.push_back(fftPlan->outStride[index]);
            }

            col2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            col2Plan->originalLength  = fftPlan->originalLength;
            col2Plan->acc  = fftPlan->acc;
            col2Plan->exist  = fftPlan->exist;
            col2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            hcfftBakePlanInternal(fftPlan->planY);
          } else {
            if( (fftPlan->length[0] > 262144 / width(fftPlan->precision)) && fftPlan->blockCompute ) {
              assert(fftPlan->length[0] <= 1048576);
              size_t padding = 64;

              if (fftPlan->tmpBufSize == 0 ) {
                fftPlan->tmpBufSize = (length1 + padding) * length0 *
                                      fftPlan->batchSize * fftPlan->ElementSize();

                for (size_t index = 1; index < fftPlan->length.size(); index++) {
                  fftPlan->tmpBufSize *= fftPlan->length[index];
                }
              }

              // Algorithm in this case is
              // T(with pad, out_of_place), R (in_place), C(in_place), Unpad(out_of_place)
              size_t len[3] = { hcLengths[1], hcLengths[0], 1 };
              hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, len );
              FFTPlan* trans1Plan = NULL;
              lockRAII* trans1Lock  = NULL;
              fftRepo.getPlan( fftPlan->planTX, trans1Plan, trans1Lock );
              trans1Plan->location     = HCFFT_OUTOFPLACE;
              trans1Plan->precision     = fftPlan->precision;
              trans1Plan->tmpBufSize    = 0;
              trans1Plan->batchSize     = fftPlan->batchSize;
              trans1Plan->envelope    = fftPlan->envelope;
              trans1Plan->ipLayout   = fftPlan->ipLayout;
              trans1Plan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
              trans1Plan->inStride[0]   = fftPlan->inStride[0];
              trans1Plan->inStride[1]   = length1;
              trans1Plan->outStride[0]  = 1;
              trans1Plan->outStride[1]  = length0 + padding;
              trans1Plan->iDist         = fftPlan->iDist;
              trans1Plan->oDist         = length1 * trans1Plan->outStride[1];
              trans1Plan->gen           = Transpose_GCN;
              trans1Plan->transflag     = true;

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                trans1Plan->length.push_back(fftPlan->length[index]);
                trans1Plan->inStride.push_back(fftPlan->inStride[index]);
                trans1Plan->outStride.push_back(trans1Plan->oDist);
                trans1Plan->oDist *= fftPlan->length[index];
              }

              trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
              trans1Plan->originalLength  = fftPlan->originalLength;
              trans1Plan->acc  = fftPlan->acc;
              trans1Plan->exist  = fftPlan->exist;
              trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planTX);
              // row FFT
              hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &hcLengths[0] );
              FFTPlan* rowPlan  = NULL;
              lockRAII* rowLock = NULL;
              fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
              assert(fftPlan->large1D == 0);
              rowPlan->location     = HCFFT_INPLACE;
              rowPlan->precision     = fftPlan->precision;
              rowPlan->forwardScale  = 1.0f;
              rowPlan->backwardScale = 1.0f;
              rowPlan->tmpBufSize    = 0;
              rowPlan->batchSize     = fftPlan->batchSize;
              rowPlan->gen      = fftPlan->gen;
              rowPlan->envelope   = fftPlan->envelope;
              rowPlan->length.push_back(length1);
              rowPlan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
              rowPlan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
              rowPlan->inStride[0]   = 1;
              rowPlan->outStride[0]  = 1;
              rowPlan->inStride.push_back(length0 + padding);
              rowPlan->outStride.push_back(length0 + padding);
              rowPlan->iDist         = (length0 + padding) * length1;
              rowPlan->oDist         = (length0 + padding) * length1;

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                rowPlan->length.push_back(fftPlan->length[index]);
                rowPlan->inStride.push_back(rowPlan->iDist);
                rowPlan->iDist *= fftPlan->length[index];
                rowPlan->outStride.push_back(rowPlan->oDist);
                rowPlan->oDist *= fftPlan->length[index];
              }

              rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
              rowPlan->originalLength  = fftPlan->originalLength;
              rowPlan->acc  = fftPlan->acc;
              rowPlan->exist  = fftPlan->exist;
              rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planX);
              //column FFT
              hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D,  &hcLengths[1] );
              FFTPlan* col2Plan = NULL;
              lockRAII* colLock = NULL;
              fftRepo.getPlan( fftPlan->planY, col2Plan, colLock );
              col2Plan->location     = HCFFT_INPLACE;
              col2Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
              col2Plan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
              col2Plan->precision     = fftPlan->precision;
              col2Plan->forwardScale  = fftPlan->forwardScale;
              col2Plan->backwardScale = fftPlan->backwardScale;
              col2Plan->tmpBufSize    = 0;
              col2Plan->batchSize     = fftPlan->batchSize;
              col2Plan->gen     = fftPlan->gen;
              col2Plan->envelope    = fftPlan->envelope;
              col2Plan->large1D       = fftPlan->length[0];
              col2Plan->twiddleFront  = true;
              col2Plan->length.push_back(hcLengths[0]);
              col2Plan->blockCompute = true;
              col2Plan->blockComputeType = BCT_C2C;
              col2Plan->inStride[0]  = length0 + padding;
              col2Plan->outStride[0] = length0 + padding;
              col2Plan->iDist        = (length0 + padding) * length1;
              col2Plan->oDist        = (length0 + padding) * length1;
              col2Plan->inStride.push_back(1);
              col2Plan->outStride.push_back(1);

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                col2Plan->length.push_back(fftPlan->length[index]);
                col2Plan->inStride.push_back(col2Plan->iDist);
                col2Plan->outStride.push_back(col2Plan->oDist);
                col2Plan->iDist   *= fftPlan->length[index];
                col2Plan->oDist   *= fftPlan->length[index];
              }

              col2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
              col2Plan->originalLength  = fftPlan->originalLength;
              col2Plan->acc  = fftPlan->acc;
              col2Plan->exist  = fftPlan->exist;
              col2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planY);
              // copy plan to get results back to packed output
              hcfftCreateDefaultPlanInternal( &fftPlan->planCopy, HCFFT_1D,  &hcLengths[0] );
              FFTPlan* copyPlan = NULL;
              lockRAII* copyLock  = NULL;
              fftRepo.getPlan( fftPlan->planCopy, copyPlan, copyLock );
              copyPlan->location     = HCFFT_OUTOFPLACE;
              copyPlan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
              copyPlan->opLayout  = fftPlan->opLayout;
              copyPlan->precision     = fftPlan->precision;
              copyPlan->forwardScale  = 1.0f;
              copyPlan->backwardScale = 1.0f;
              copyPlan->tmpBufSize    = 0;
              copyPlan->batchSize     = fftPlan->batchSize;
              copyPlan->gen     = Copy;
              copyPlan->envelope    = fftPlan->envelope;
              copyPlan->length.push_back(length1);
              copyPlan->inStride[0]  = 1;
              copyPlan->inStride.push_back(length0 + padding);
              copyPlan->iDist        = length1 * (length0 + padding);
              copyPlan->outStride[0] = fftPlan->outStride[0];
              copyPlan->outStride.push_back(length0);
              copyPlan->oDist         = fftPlan->oDist;

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                copyPlan->length.push_back(fftPlan->length[index]);
                copyPlan->inStride.push_back(copyPlan->inStride[index] * copyPlan->length[index]);
                copyPlan->iDist   *= fftPlan->length[index];
                copyPlan->outStride.push_back(fftPlan->outStride[index]);
              }

              copyPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
              copyPlan->originalLength  = fftPlan->originalLength;
              copyPlan->acc  = fftPlan->acc;
              copyPlan->exist  = fftPlan->exist;
              copyPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planCopy);
            } else {
              if (fftPlan->tmpBufSize == 0 ) {
                fftPlan->tmpBufSize = length0 * length1 *
                                      fftPlan->batchSize * fftPlan->ElementSize();

                for (size_t index = 1; index < fftPlan->length.size(); index++) {
                  fftPlan->tmpBufSize *= fftPlan->length[index];
                }
              }

              // column FFT, size hcLengths[1], batch hcLengths[0], with length[0] twiddle factor multiplication
              // transposed output
              hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &hcLengths[1] );
              FFTPlan* colTPlan = NULL;
              lockRAII* colLock = NULL;
              fftRepo.getPlan( fftPlan->planX, colTPlan, colLock );
              assert(fftPlan->large1D == 0);
              // current plan is to create intermediate buffer, packed and interleave
              // This is a column FFT, the first elements distance between each FFT is the distance of the first two
              // elements in the original buffer. Like a transpose of the matrix
              // we need to pass hcLengths[0] and instride size to kernel, so kernel can tell the difference
              //this part are common for both passes
              colTPlan->location     = HCFFT_OUTOFPLACE;
              colTPlan->precision     = fftPlan->precision;
              colTPlan->forwardScale  = 1.0f;
              colTPlan->backwardScale = 1.0f;
              colTPlan->tmpBufSize    = 0;
              colTPlan->batchSize     = fftPlan->batchSize;
              colTPlan->gen     = fftPlan->gen;
              colTPlan->envelope      = fftPlan->envelope;
              //Pass large1D flag to confirm we need multiply twiddle factor
              colTPlan->large1D       = fftPlan->length[0];
              colTPlan->length.push_back(length0);
              colTPlan->ipLayout   = fftPlan->ipLayout;
              colTPlan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
              colTPlan->inStride[0]   = fftPlan->inStride[0] * length0;
              colTPlan->outStride[0]  = length0;
              colTPlan->iDist         = fftPlan->iDist;
              colTPlan->oDist         = length0 * length1;
              colTPlan->inStride.push_back(fftPlan->inStride[0]);
              colTPlan->outStride.push_back(1);

              // Enabling block column compute
              if( (colTPlan->inStride[0] == length0) && IsPo2(fftPlan->length[0]) && (fftPlan->length[0] < 524288) ) {
                colTPlan->blockCompute = true;
                colTPlan->blockComputeType = BCT_C2C;
              }

              for (size_t index = 1; index < fftPlan->length.size(); index++) {
                colTPlan->length.push_back(fftPlan->length[index]);
                colTPlan->inStride.push_back(fftPlan->inStride[index]);
                // tmp buffer is tightly packed
                colTPlan->outStride.push_back(colTPlan->oDist);
                colTPlan->oDist        *= fftPlan->length[index];
              }

              colTPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
              colTPlan->originalLength  = fftPlan->originalLength;
              colTPlan->acc  = fftPlan->acc;
              colTPlan->exist  = fftPlan->exist;
              colTPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planX);
              //another column FFT, size hcLengths[0], batch hcLengths[1], output without transpose
              hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D,  &hcLengths[0] );
              FFTPlan* col2Plan = NULL;
              lockRAII* rowLock = NULL;
              fftRepo.getPlan( fftPlan->planY, col2Plan, rowLock );
              // This is second column fft, intermediate buffer is packed and interleaved
              // we need to pass hcLengths[1] and instride size to kernel, so kernel can tell the difference
              // common part for both passes
              col2Plan->opLayout  = fftPlan->opLayout;
              col2Plan->precision     = fftPlan->precision;
              col2Plan->forwardScale  = fftPlan->forwardScale;
              col2Plan->backwardScale = fftPlan->backwardScale;
              col2Plan->tmpBufSize    = 0;
              col2Plan->batchSize     = fftPlan->batchSize;
              col2Plan->oDist         = fftPlan->oDist;
              col2Plan->gen     = fftPlan->gen;
              col2Plan->envelope    = fftPlan->envelope;
              col2Plan->length.push_back(hcLengths[1]);
              bool integratedTranposes = true;

              if( colTPlan->blockCompute && (fftPlan->outStride[0] == 1) && hcLengths[0] <= 256) {
                col2Plan->blockCompute = true;
                col2Plan->blockComputeType = BCT_R2C;
                col2Plan->location    = HCFFT_OUTOFPLACE;
                col2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                col2Plan->inStride[0]  = 1;
                col2Plan->outStride[0] = length1;
                col2Plan->iDist        = length0 * length1;
                col2Plan->inStride.push_back(length0);
                col2Plan->outStride.push_back(1);
              } else if( colTPlan->blockCompute && (fftPlan->outStride[0] == 1) ) {
                integratedTranposes = false;
                col2Plan->location    = HCFFT_INPLACE;
                col2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                col2Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                col2Plan->inStride[0]  = 1;
                col2Plan->outStride[0] = 1;
                col2Plan->iDist        = length0 * length1;
                col2Plan->oDist        = length0 * length1;
                col2Plan->inStride.push_back(length0);
                col2Plan->outStride.push_back(length0);
              } else {
                //first layer, large 1D from tmp buffer to output buffer
                col2Plan->location    = HCFFT_OUTOFPLACE;
                col2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                col2Plan->inStride[0]  = 1;
                col2Plan->outStride[0] = fftPlan->outStride[0] * hcLengths[1];
                col2Plan->iDist        = length0 * length1; //fftPlan->length[0];
                col2Plan->inStride.push_back(length0);
                col2Plan->outStride.push_back(fftPlan->outStride[0]);
              }

              if(!integratedTranposes) {
                for (size_t index = 1; index < fftPlan->length.size(); index++) {
                  col2Plan->length.push_back(fftPlan->length[index]);
                  col2Plan->inStride.push_back(col2Plan->iDist);
                  col2Plan->outStride.push_back(col2Plan->oDist);
                  col2Plan->iDist        *= fftPlan->length[index];
                  col2Plan->oDist        *= fftPlan->length[index];
                }
              } else {
                for (size_t index = 1; index < fftPlan->length.size(); index++) {
                  col2Plan->length.push_back(fftPlan->length[index]);
                  col2Plan->inStride.push_back(col2Plan->iDist);
                  col2Plan->outStride.push_back(fftPlan->outStride[index]);
                  col2Plan->iDist   *= fftPlan->length[index];
                }
              }

              col2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
              col2Plan->originalLength  = fftPlan->originalLength;
              col2Plan->acc  = fftPlan->acc;
              col2Plan->exist  = fftPlan->exist;
              col2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
              hcfftBakePlanInternal(fftPlan->planY);

              if(!integratedTranposes) {
                //Transpose
                //tmp --> output
                hcfftCreateDefaultPlanInternal( &fftPlan->planTZ, HCFFT_2D, hcLengths );
                FFTPlan* trans3Plan = NULL;
                lockRAII* trans3Lock  = NULL;
                fftRepo.getPlan( fftPlan->planTZ, trans3Plan, trans3Lock );
                trans3Plan->location     = HCFFT_OUTOFPLACE;
                trans3Plan->precision     = fftPlan->precision;
                trans3Plan->tmpBufSize    = 0;
                trans3Plan->batchSize     = fftPlan->batchSize;
                trans3Plan->envelope    = fftPlan->envelope;
                trans3Plan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
                trans3Plan->opLayout  = fftPlan->opLayout;
                trans3Plan->inStride[0]   = 1;
                trans3Plan->inStride[1]   = hcLengths[0];
                trans3Plan->outStride[0]  = fftPlan->outStride[0];
                trans3Plan->outStride[1]  = hcLengths[1] * fftPlan->outStride[0];
                trans3Plan->iDist         = fftPlan->length[0];
                trans3Plan->oDist         = fftPlan->oDist;
                trans3Plan->gen           = Transpose_GCN;
                trans3Plan->transflag     = true;

                for (size_t index = 1; index < fftPlan->length.size(); index++) {
                  trans3Plan->length.push_back(fftPlan->length[index]);
                  trans3Plan->inStride.push_back(trans3Plan->iDist);
                  trans3Plan->iDist *= fftPlan->length[index];
                  trans3Plan->outStride.push_back(fftPlan->outStride[index]);
                }

                trans3Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
                trans3Plan->originalLength  = fftPlan->originalLength;
                trans3Plan->acc  = fftPlan->acc;
                trans3Plan->exist  = fftPlan->exist;
                trans3Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
                hcfftBakePlanInternal(fftPlan->planTZ);
              }
            }
          }

          fftPlan->baked = true;
          return  HCFFT_SUCCEEDS;
        }
      }
      break;

    case HCFFT_2D: {
        if (fftPlan->transflag) { //Transpose for 2D
          if(fftPlan->gen == Transpose_GCN) {
            fftPlan->GenerateKernel(plHandle, fftRepo, bakedPlanCount, fftPlan->exist);
            CompileKernels(plHandle, fftPlan->gen, fftPlan, fftPlan->plHandleOrigin, fftPlan->exist, fftPlan->originalLength, fftPlan->hcfftlibtype);
          } else if (fftPlan->gen == Transpose_SQUARE) {
            fftPlan->GenerateKernel(plHandle, fftRepo, bakedPlanCount, fftPlan->exist);
            CompileKernels(plHandle, fftPlan->gen, fftPlan, fftPlan->plHandleOrigin, fftPlan->exist, fftPlan->originalLength, fftPlan->hcfftlibtype);
          } else if (fftPlan->gen == Transpose_NONSQUARE) {
            if(fftPlan->nonSquareKernelType != NON_SQUARE_TRANS_PARENT) {
              fftPlan->GenerateKernel(plHandle, fftRepo, bakedPlanCount, fftPlan->exist);
              CompileKernels(plHandle, fftPlan->gen, fftPlan, fftPlan->plHandleOrigin, fftPlan->exist, fftPlan->originalLength, fftPlan->hcfftlibtype);
            } else {
              size_t hcLengths[] = { 1, 1, 0 };
              hcLengths[0] = fftPlan->length[0];
              hcLengths[1] = fftPlan->length[1];

              //NON_SQUARE_KERNEL_ORDER currKernelOrder;
              // controling the transpose and swap kernel order
              // if leading dim is larger than the other dim it makes sense to swap and transpose
              if (hcLengths[0] > hcLengths[1]) {
                //Twiddling will be done in swap kernel, in regardless of the order
                fftPlan->nonSquareKernelOrder = SWAP_AND_TRANSPOSE;
              } else {
                if (fftPlan->large1D != 0 && 0) {
                  //this is not going to happen anymore
                  fftPlan->nonSquareKernelOrder = TRANSPOSE_LEADING_AND_SWAP;
                } else {
                  //twiddling can be done in swap
                  fftPlan->nonSquareKernelOrder = TRANSPOSE_AND_SWAP;
                }
              }

              //std::cout << "currKernelOrder = " << fftPlan->nonSquareKernelOrder << std::endl;
              //ends tranpose kernel order
              //Transpose stage 1
              hcfftCreateDefaultPlanInternal(&fftPlan->planTX, HCFFT_2D, hcLengths);
              FFTPlan* trans1Plan = NULL;
              lockRAII* trans1Lock = NULL;
              fftRepo.getPlan(fftPlan->planTX, trans1Plan, trans1Lock);
              trans1Plan->location = HCFFT_INPLACE;
              trans1Plan->precision = fftPlan->precision;
              trans1Plan->tmpBufSize = 0;
              trans1Plan->batchSize = fftPlan->batchSize;
              trans1Plan->envelope = fftPlan->envelope;
              trans1Plan->ipLayout = fftPlan->ipLayout;
              trans1Plan->opLayout = fftPlan->opLayout;
              trans1Plan->inStride[0] = fftPlan->inStride[0];
              trans1Plan->outStride[0] = fftPlan->outStride[0];
              trans1Plan->inStride[1] = fftPlan->inStride[1];
              trans1Plan->outStride[1] = fftPlan->outStride[1];
              trans1Plan->iDist = fftPlan->iDist;
              trans1Plan->oDist = fftPlan->oDist;
              trans1Plan->gen = Transpose_NONSQUARE;
              trans1Plan->nonSquareKernelOrder = fftPlan->nonSquareKernelOrder;

              if(fftPlan->nonSquareKernelOrder == SWAP_AND_TRANSPOSE) {
                trans1Plan->nonSquareKernelType = NON_SQUARE_TRANS_SWAP;
              } else if (fftPlan->nonSquareKernelOrder == TRANSPOSE_AND_SWAP) {
                trans1Plan->nonSquareKernelType = NON_SQUARE_TRANS_TRANSPOSE_BATCHED;
              } else if(fftPlan->nonSquareKernelOrder == TRANSPOSE_LEADING_AND_SWAP) {
                trans1Plan->nonSquareKernelType = NON_SQUARE_TRANS_TRANSPOSE_BATCHED_LEADING;
              }

              trans1Plan->transflag = true;
              trans1Plan->large1D = fftPlan->large1D;//twiddling may happen in this kernel
              trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
              trans1Plan->originalLength  = fftPlan->originalLength;
              trans1Plan->acc  = fftPlan->acc;
              trans1Plan->exist  = fftPlan->exist;
              trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

              if (trans1Plan->nonSquareKernelType == NON_SQUARE_TRANS_TRANSPOSE_BATCHED) {
                //this should be in a function to avoide duplicate code TODO
                //need to treat a non square matrix as a sqaure matrix with bigger batch size
                size_t lengthX = trans1Plan->length[0];
                size_t lengthY = trans1Plan->length[1];
                size_t BatchFactor = (lengthX > lengthY) ? (lengthX / lengthY) : (lengthY / lengthX);
                trans1Plan->transposeMiniBatchSize = BatchFactor;
                trans1Plan->batchSize *= BatchFactor;
                trans1Plan->iDist = trans1Plan->iDist / BatchFactor;

                if (lengthX > lengthY) {
                  trans1Plan->length[0] = lengthX / BatchFactor;
                  trans1Plan->inStride[1] = lengthX / BatchFactor;
                } else if (lengthX < lengthY) {
                  trans1Plan->length[1] = lengthY / BatchFactor;
                  trans1Plan->inStride[1] = lengthX;
                }
              }

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                trans1Plan->length.push_back(fftPlan->length[index]);
                trans1Plan->inStride.push_back(fftPlan->inStride[index]);
                trans1Plan->outStride.push_back(fftPlan->outStride[index]);
              }

              hcfftBakePlanInternal(fftPlan->planTX);
              //Transpose stage 2
              hcfftCreateDefaultPlanInternal(&fftPlan->planTY, HCFFT_2D, hcLengths);
              FFTPlan* trans2Plan = NULL;
              lockRAII* trans2Lock = NULL;
              fftRepo.getPlan(fftPlan->planTY, trans2Plan, trans2Lock);
              trans2Plan->location = HCFFT_INPLACE;
              trans2Plan->precision = fftPlan->precision;
              trans2Plan->tmpBufSize = 0;
              trans2Plan->batchSize = fftPlan->batchSize;
              trans2Plan->envelope = fftPlan->envelope;
              trans2Plan->ipLayout = fftPlan->ipLayout;
              trans2Plan->opLayout = fftPlan->opLayout;
              trans2Plan->inStride[0] = fftPlan->inStride[0];
              trans2Plan->outStride[0] = fftPlan->outStride[0];
              trans2Plan->inStride[1] = fftPlan->inStride[1];
              trans2Plan->outStride[1] = fftPlan->outStride[1];
              trans2Plan->iDist = fftPlan->iDist;
              trans2Plan->oDist = fftPlan->oDist;
              trans2Plan->gen = Transpose_NONSQUARE;
              trans2Plan->nonSquareKernelOrder = fftPlan->nonSquareKernelOrder;

              if (fftPlan->nonSquareKernelOrder == SWAP_AND_TRANSPOSE) {
                trans2Plan->nonSquareKernelType = NON_SQUARE_TRANS_TRANSPOSE_BATCHED;
              } else if(fftPlan->nonSquareKernelOrder == TRANSPOSE_AND_SWAP) {
                trans2Plan->nonSquareKernelType = NON_SQUARE_TRANS_SWAP;
              } else if(fftPlan->nonSquareKernelOrder == TRANSPOSE_LEADING_AND_SWAP) {
                trans2Plan->nonSquareKernelType = NON_SQUARE_TRANS_SWAP;
              }

              trans2Plan->transflag = true;
              trans2Plan->large1D = fftPlan->large1D;//twiddling may happen in this kernel
              trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
              trans2Plan->originalLength  = fftPlan->originalLength;
              trans2Plan->acc  = fftPlan->acc;
              trans2Plan->exist  = fftPlan->exist;
              trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;

              if (trans2Plan->nonSquareKernelType == NON_SQUARE_TRANS_TRANSPOSE_BATCHED) {
                //need to treat a non square matrix as a sqaure matrix with bigger batch size
                size_t lengthX = trans2Plan->length[0];
                size_t lengthY = trans2Plan->length[1];
                size_t BatchFactor = (lengthX > lengthY) ? (lengthX / lengthY) : (lengthY / lengthX);
                trans2Plan->transposeMiniBatchSize = BatchFactor;
                trans2Plan->batchSize *= BatchFactor;
                trans2Plan->iDist = trans2Plan->iDist / BatchFactor;

                if (lengthX > lengthY) {
                  trans2Plan->length[0] = lengthX / BatchFactor;
                  trans2Plan->inStride[1] = lengthX / BatchFactor;
                } else if(lengthX < lengthY) {
                  trans2Plan->length[1] = lengthY / BatchFactor;
                  trans2Plan->inStride[1] = lengthX;
                }
              }

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                trans2Plan->length.push_back(fftPlan->length[index]);
                trans2Plan->inStride.push_back(fftPlan->inStride[index]);
                trans2Plan->outStride.push_back(fftPlan->outStride[index]);
              }

              hcfftBakePlanInternal(fftPlan->planTY);
            }
          } else {
            fftPlan->GenerateKernel(plHandle, fftRepo, bakedPlanCount, fftPlan->exist);
            CompileKernels(plHandle, fftPlan->gen, fftPlan, fftPlan->plHandleOrigin, fftPlan->exist, fftPlan->originalLength, fftPlan->hcfftlibtype);
          }

          fftPlan->baked    = true;
          bakedPlanCount++;
          return  HCFFT_SUCCEEDS;
        }

        size_t length0 = fftPlan->length[0];
        size_t length1 = fftPlan->length[1];

        if (fftPlan->length[0] > Large1DThreshold ||
            fftPlan->length[1] > Large1DThreshold) {
          fftPlan->large2D = true;
        }

        while (1 && (fftPlan->ipLayout != HCFFT_REAL) && (fftPlan->opLayout != HCFFT_REAL)) {
          //break;
          if (fftPlan->length.size() != 2) {
            break;
          }

          if (!(IsPo2(fftPlan->length[0])) || !(IsPo2(fftPlan->length[1]))) {
            break;
          }

          if (fftPlan->length[1] < 32) {
            break;
          }

          if (fftPlan->length[0] < 64) {
            break;
          }

          //x!=y case, we need tmp buffer, currently temp buffer only support interleaved format
          //if (fftPlan->length[0] != fftPlan->length[1] && fftPlan->opLayout == HCFFT_COMPLEX_PLANAR) break;
          if (fftPlan->inStride[0] != 1 || fftPlan->outStride[0] != 1 ||
              fftPlan->inStride[1] != fftPlan->length[0] || fftPlan->outStride[1] != fftPlan->length[0]) {
            break;
          }

          //if (fftPlan->location != HCFFT_INPLACE || fftPlan->ipLayout != HCFFT_COMPLEX_PLANAR)
          //  break;
          //if (fftPlan->batchSize != 1) break;
          //if (fftPlan->precision != HCFFT_SINGLE) break;
          fftPlan->transflag = true;
          //create row plan,
          // x=y & x!=y, In->In for inplace, In->out for outofplace
          hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &fftPlan->length[ 0 ]);
          FFTPlan* rowPlan  = NULL;
          lockRAII* rowLock = NULL;
          fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
          rowPlan->ipLayout     = fftPlan->ipLayout;
          rowPlan->opLayout    = fftPlan->opLayout;
          rowPlan->location       = fftPlan->location;
          rowPlan->outStride[0]    = fftPlan->outStride[0];
          rowPlan->outStride.push_back(fftPlan->outStride[1]);
          rowPlan->oDist           = fftPlan->oDist;
          rowPlan->precision       = fftPlan->precision;
          rowPlan->forwardScale    = 1.0f;
          rowPlan->backwardScale   = 1.0f;
          rowPlan->tmpBufSize      = 0;
          rowPlan->gen       = fftPlan->gen;
          rowPlan->envelope    = fftPlan->envelope;
          rowPlan->batchSize       = fftPlan->batchSize;
          rowPlan->inStride[0]     = fftPlan->inStride[0];
          rowPlan->length.push_back(fftPlan->length[1]);
          rowPlan->inStride.push_back(fftPlan->inStride[1]);
          rowPlan->iDist           = fftPlan->iDist;
          rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          rowPlan->originalLength  = fftPlan->originalLength;
          rowPlan->acc  = fftPlan->acc;
          rowPlan->exist  = fftPlan->exist;
          rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          hcfftBakePlanInternal(fftPlan->planX);
          //Create transpose plan for first transpose
          //x=y: inplace. x!=y inplace: in->tmp, outofplace out->tmp
          size_t hcLengths[] = { 1, 1, 0 };
          hcLengths[0] = fftPlan->length[0];
          hcLengths[1] = fftPlan->length[1];
          size_t biggerDim = hcLengths[0] > hcLengths[1] ? hcLengths[0] : hcLengths[1];
          size_t smallerDim = biggerDim == hcLengths[0] ? hcLengths[1] : hcLengths[0];
          size_t padding = 0;
          fftPlan->transpose_in_2d_inplace = (hcLengths[0] == hcLengths[1]) ? true : false;

          if ( (!fftPlan->transpose_in_2d_inplace) && fftPlan->tmpBufSize == 0 && fftPlan->length.size() <= 2 ) {
            if ((smallerDim % 64 == 0) || (biggerDim % 64 == 0))
              if(biggerDim > 512) {
                padding = 64;
              }

            // we need tmp buffer for x!=y case
            // we assume the tmp buffer is packed interleaved
            fftPlan->tmpBufSize = (smallerDim + padding) * biggerDim *
                                  fftPlan->batchSize * fftPlan->ElementSize();
          }

          hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, hcLengths);
          FFTPlan* transPlanX = NULL;
          lockRAII* transLockX  = NULL;
          fftRepo.getPlan( fftPlan->planTX, transPlanX, transLockX );
          transPlanX->ipLayout     = fftPlan->opLayout;
          transPlanX->precision       = fftPlan->precision;
          transPlanX->tmpBufSize      = 0;
          transPlanX->envelope    = fftPlan->envelope;
          transPlanX->batchSize       = fftPlan->batchSize;
          transPlanX->inStride[0]     = fftPlan->outStride[0];
          transPlanX->inStride[1]     = fftPlan->outStride[1];
          transPlanX->iDist           = fftPlan->oDist;
          transPlanX->transflag       = true;

          if (!fftPlan->transpose_in_2d_inplace) {
            transPlanX->gen         = Transpose_GCN;
            transPlanX->opLayout    = HCFFT_COMPLEX_INTERLEAVED;
            transPlanX->location       = HCFFT_OUTOFPLACE;
            transPlanX->outStride[0]    = 1;
            transPlanX->outStride[1]    = hcLengths[1] + padding;
            transPlanX->oDist           = hcLengths[0] * transPlanX->outStride[1];
          } else {
            transPlanX->gen = Transpose_SQUARE;
            transPlanX->opLayout    = fftPlan->opLayout;
            transPlanX->location       = HCFFT_INPLACE;
            transPlanX->outStride[0]    = fftPlan->outStride[0];
            transPlanX->outStride[1]    = fftPlan->outStride[1];
            transPlanX->oDist           = fftPlan->oDist;
          }

          transPlanX->hcfftlibtype  = fftPlan->hcfftlibtype;
          transPlanX->originalLength  = fftPlan->originalLength;
          transPlanX->acc  = fftPlan->acc;
          transPlanX->exist  = fftPlan->exist;
          transPlanX->plHandleOrigin  = fftPlan->plHandleOrigin;
          hcfftBakePlanInternal(fftPlan->planTX);
          //create second row plan
          //x!=y: tmp->tmp, x=y case: In->In or Out->Out
          //if Transposed result is a choice x!=y: tmp->In or out
          hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &fftPlan->length[ 1 ]);
          FFTPlan* colPlan  = NULL;
          lockRAII* colLock = NULL;
          fftRepo.getPlan( fftPlan->planY, colPlan, colLock );

          if (!fftPlan->transpose_in_2d_inplace) {
            colPlan->ipLayout     = HCFFT_COMPLEX_INTERLEAVED;
            colPlan->inStride[0]     = 1;
            colPlan->inStride.push_back(hcLengths[1] + padding);
            colPlan->iDist           = hcLengths[0] * colPlan->inStride[1];

            if (fftPlan->transposeType == HCFFT_NOTRANSPOSE) {
              colPlan->opLayout    = HCFFT_COMPLEX_INTERLEAVED;
              colPlan->outStride[0]    = 1;
              colPlan->outStride.push_back(hcLengths[1] + padding);
              colPlan->oDist           = hcLengths[0] * colPlan->outStride[1];
              colPlan->location       = HCFFT_INPLACE;
            } else {
              colPlan->opLayout    = fftPlan->opLayout;
              colPlan->outStride[0]    = fftPlan->outStride[0];
              colPlan->outStride.push_back(hcLengths[1] * fftPlan->outStride[0]);
              colPlan->oDist           = fftPlan->oDist;
              colPlan->location       = HCFFT_OUTOFPLACE;
            }
          } else {
            colPlan->ipLayout     = fftPlan->opLayout;
            colPlan->opLayout    = fftPlan->opLayout;
            colPlan->outStride[0]    = fftPlan->outStride[0];
            colPlan->outStride.push_back(fftPlan->outStride[1]);
            colPlan->oDist           = fftPlan->oDist;
            colPlan->inStride[0]     = fftPlan->outStride[0];
            colPlan->inStride.push_back(fftPlan->outStride[1]);
            colPlan->iDist           = fftPlan->oDist;
            colPlan->location       = HCFFT_INPLACE;
          }

          colPlan->precision       = fftPlan->precision;
          colPlan->forwardScale    = fftPlan->forwardScale;
          colPlan->backwardScale   = fftPlan->backwardScale;
          colPlan->tmpBufSize      = 0;
          colPlan->gen       = fftPlan->gen;
          colPlan->envelope    = fftPlan->envelope;
          colPlan->batchSize       = fftPlan->batchSize;
          colPlan->length.push_back(fftPlan->length[0]);
          colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          colPlan->originalLength  = fftPlan->originalLength;
          colPlan->acc  = fftPlan->acc;
          colPlan->exist  = fftPlan->exist;
          colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          hcfftBakePlanInternal(fftPlan->planY);

          if (fftPlan->transposeType == HCFFT_TRANSPOSED) {
            fftPlan->baked = true;
            return  HCFFT_SUCCEEDS;
          }

          //Create transpose plan for second transpose
          //x!=y case tmp->In or Out, x=y case In->In or Out->out
          size_t hcLengthsY[2] = { hcLengths[1], hcLengths[0] };
          hcfftCreateDefaultPlanInternal( &fftPlan->planTY, HCFFT_2D, hcLengthsY );
          FFTPlan* transPlanY = NULL;
          lockRAII* transLockY  = NULL;
          fftRepo.getPlan( fftPlan->planTY, transPlanY, transLockY );

          if (!fftPlan->transpose_in_2d_inplace) {
            transPlanY->gen = Transpose_GCN;
            transPlanY->ipLayout     = HCFFT_COMPLEX_INTERLEAVED;
            transPlanY->location       = HCFFT_OUTOFPLACE;
            transPlanY->inStride[0]     = 1;
            transPlanY->inStride[1]     = hcLengths[1] + padding;
            transPlanY->iDist           = hcLengths[0] * transPlanY->inStride[1];
            transPlanY->transOutHorizontal = true;
          } else {
            transPlanY->gen = Transpose_SQUARE;
            transPlanY->ipLayout     = fftPlan->opLayout;
            transPlanY->location       = HCFFT_INPLACE;
            transPlanY->inStride[0]     = fftPlan->outStride[0];
            transPlanY->inStride[1]     = fftPlan->outStride[1];
            transPlanY->iDist           = fftPlan->oDist;
          }

          transPlanY->opLayout    = fftPlan->opLayout;
          transPlanY->outStride[0]    = fftPlan->outStride[0];
          transPlanY->outStride[1]    = fftPlan->outStride[1];
          transPlanY->oDist           = fftPlan->oDist;
          transPlanY->precision       = fftPlan->precision;
          transPlanY->tmpBufSize      = 0;
          transPlanY->envelope      = fftPlan->envelope;
          transPlanY->batchSize       = fftPlan->batchSize;
          transPlanY->transflag       = true;
          transPlanY->hcfftlibtype  = fftPlan->hcfftlibtype;
          transPlanY->originalLength  = fftPlan->originalLength;
          transPlanY->acc  = fftPlan->acc;
          transPlanY->exist  = fftPlan->exist;
          transPlanY->plHandleOrigin  = fftPlan->plHandleOrigin;
          hcfftBakePlanInternal(fftPlan->planTY);
          fftPlan->baked = true;
          return  HCFFT_SUCCEEDS;
        }

        //check transposed
        if (fftPlan->transposeType != HCFFT_NOTRANSPOSE) {
          return HCFFT_ERROR;
        }

        if(fftPlan->ipLayout == HCFFT_REAL) {
          length0 = fftPlan->length[0];
          length1 = fftPlan->length[1];
          size_t Nt = (1 + length0 / 2);
          // create row plan
          // real to hermitian
          //create row plan
          hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &fftPlan->length[ 0 ]);
          FFTPlan* rowPlan  = NULL;
          lockRAII* rowLock = NULL;
          fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
          rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          rowPlan->opLayout  = fftPlan->opLayout;
          rowPlan->ipLayout  = fftPlan->ipLayout;
          rowPlan->location     = fftPlan->location;
          rowPlan->length.push_back(length1);
          rowPlan->inStride[0]  = fftPlan->inStride[0];
          rowPlan->inStride.push_back(fftPlan->inStride[1]);
          rowPlan->iDist         = fftPlan->iDist;
          rowPlan->precision     = fftPlan->precision;
          rowPlan->forwardScale  = 1.0f;
          rowPlan->backwardScale = 1.0f;
          rowPlan->tmpBufSize    = 0;
          rowPlan->gen      = fftPlan->gen;
          rowPlan->envelope   = fftPlan->envelope;
          rowPlan->batchSize    = fftPlan->batchSize;
          rowPlan->outStride[0]  = fftPlan->outStride[0];
          rowPlan->outStride.push_back(fftPlan->outStride[1]);
          rowPlan->oDist         = fftPlan->oDist;

          //this 2d is decomposed from 3d
          for (size_t index = 2; index < fftPlan->length.size(); index++) {
            rowPlan->length.push_back(fftPlan->length[index]);
            rowPlan->inStride.push_back(fftPlan->inStride[index]);
            rowPlan->outStride.push_back(fftPlan->outStride[index]);
          }

          rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          rowPlan->originalLength  = fftPlan->originalLength;
          rowPlan->acc  = fftPlan->acc;
          rowPlan->exist  = fftPlan->exist;
          hcfftBakePlanInternal(fftPlan->planX);

          if( (rowPlan->inStride[0] == 1) && (rowPlan->outStride[0] == 1) &&
              ( ((rowPlan->inStride[1] == Nt * 2) && (rowPlan->location == HCFFT_INPLACE)) ||
                ((rowPlan->inStride[1] == length0) && (rowPlan->location == HCFFT_OUTOFPLACE)) )
              && (rowPlan->outStride[1] == Nt) ) {
            // calc temp buf size
            if (fftPlan->tmpBufSize == 0) {
              fftPlan->tmpBufSize = Nt * length1 * fftPlan->batchSize * fftPlan->ElementSize();

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                fftPlan->tmpBufSize *= fftPlan->length[index];
              }
            }

            // create first transpose plan
            //Transpose
            // output --> tmp
            size_t transLengths[2] = { length0, length1 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, transLengths );
            FFTPlan* trans1Plan = NULL;
            lockRAII* trans1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTX, trans1Plan, trans1Lock );
            trans1Plan->transflag = true;
            transLengths[0] = Nt;
            hcfftSetPlanLength( fftPlan->planTX, HCFFT_2D, transLengths );

            switch(fftPlan->opLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans1Plan->location     = HCFFT_OUTOFPLACE;
            trans1Plan->precision     = fftPlan->precision;
            trans1Plan->tmpBufSize    = 0;
            trans1Plan->batchSize     = fftPlan->batchSize;
            trans1Plan->envelope    = fftPlan->envelope;
            trans1Plan->forwardScale  = 1.0f;
            trans1Plan->backwardScale = 1.0f;
            trans1Plan->inStride[0]   = 1;
            trans1Plan->inStride[1]   = Nt;
            trans1Plan->outStride[0]  = 1;
            trans1Plan->outStride[1]  = length1;
            trans1Plan->iDist         = rowPlan->oDist;
            trans1Plan->oDist     = Nt * length1;
            trans1Plan->transOutHorizontal = true;
            trans1Plan->gen           = Transpose_GCN;

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              trans1Plan->length.push_back(fftPlan->length[index]);
              trans1Plan->inStride.push_back(rowPlan->outStride[index]);
              trans1Plan->outStride.push_back(trans1Plan->oDist);
              trans1Plan->oDist *= fftPlan->length[index];
            }

            trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans1Plan->originalLength  = fftPlan->originalLength;
            trans1Plan->acc  = fftPlan->acc;
            trans1Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTX);
            // Create column plan as a row plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &fftPlan->length[ 1 ]);
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planY, colPlan, colLock );
            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->opLayout  = trans1Plan->opLayout;
            colPlan->ipLayout   = trans1Plan->opLayout;
            colPlan->location     = HCFFT_INPLACE;
            colPlan->length.push_back(Nt);
            colPlan->inStride[0]  = 1;
            colPlan->inStride.push_back(length1);
            colPlan->iDist         = Nt * length1;
            colPlan->outStride[0]  = 1;
            colPlan->outStride.push_back(length1);
            colPlan->oDist         = Nt * length1;
            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = fftPlan->forwardScale;
            colPlan->backwardScale = fftPlan->backwardScale;
            colPlan->tmpBufSize    = 0;
            colPlan->gen      = fftPlan->gen;
            colPlan->envelope   = fftPlan->envelope;
            colPlan->batchSize    = fftPlan->batchSize;

            //this 2d is decomposed from 3d
            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              colPlan->length.push_back(fftPlan->length[index]);
              colPlan->inStride.push_back(colPlan->iDist);
              colPlan->outStride.push_back(colPlan->oDist);
              colPlan->iDist *= fftPlan->length[index];
              colPlan->oDist *= fftPlan->length[index];
            }

            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planY);

            if (fftPlan->transposeType == HCFFT_TRANSPOSED) {
              fftPlan->baked = true;
              return  HCFFT_SUCCEEDS;
            }

            // create second transpose plan
            //Transpose
            //output --> tmp
            size_t trans2Lengths[2] = { length1, length0 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTY, HCFFT_2D, trans2Lengths );
            FFTPlan* trans2Plan = NULL;
            lockRAII* trans2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTY, trans2Plan, trans2Lock );
            trans2Plan->transflag = true;
            trans2Lengths[1] = Nt;
            hcfftSetPlanLength( fftPlan->planTY, HCFFT_2D, trans2Lengths );

            switch(fftPlan->opLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  trans2Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  trans2Plan->opLayout = HCFFT_COMPLEX_PLANAR;
                  trans2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              default:
                assert(false);
            }

            trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans2Plan->location     = HCFFT_OUTOFPLACE;
            trans2Plan->precision     = fftPlan->precision;
            trans2Plan->tmpBufSize    = 0;
            trans2Plan->batchSize     = fftPlan->batchSize;
            trans2Plan->envelope    = fftPlan->envelope;
            trans2Plan->forwardScale  = 1.0f;
            trans2Plan->backwardScale = 1.0f;
            trans2Plan->inStride[0]   = 1;
            trans2Plan->inStride[1]   = length1;
            trans2Plan->outStride[0]  = 1;
            trans2Plan->outStride[1]  = Nt;
            trans2Plan->iDist         = Nt * length1;
            trans2Plan->oDist     = fftPlan->oDist;
            trans2Plan->gen           = Transpose_GCN;
            trans2Plan->transflag     = true;
            trans2Plan->acc  = fftPlan->acc;

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              trans2Plan->length.push_back(fftPlan->length[index]);
              trans2Plan->inStride.push_back(trans2Plan->iDist);
              trans2Plan->iDist *= fftPlan->length[index];
              trans2Plan->outStride.push_back(fftPlan->outStride[index]);
            }

            trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans2Plan->originalLength  = fftPlan->originalLength;
            trans2Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTY);
          } else {
            // create col plan
            // complex to complex
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &fftPlan->length[ 1 ] );
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planY, colPlan, colLock );

            switch(fftPlan->opLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  colPlan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  colPlan->opLayout = HCFFT_COMPLEX_PLANAR;
                  colPlan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            colPlan->location     = HCFFT_INPLACE;
            colPlan->length.push_back(Nt);
            colPlan->outStride[0]  = fftPlan->outStride[1];
            colPlan->outStride.push_back(fftPlan->outStride[0]);
            colPlan->oDist         = fftPlan->oDist;
            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = fftPlan->forwardScale;
            colPlan->backwardScale = fftPlan->backwardScale;
            colPlan->tmpBufSize    = fftPlan->tmpBufSize;
            colPlan->gen      = fftPlan->gen;
            colPlan->envelope     = fftPlan->envelope;
            colPlan->batchSize = fftPlan->batchSize;
            colPlan->inStride[0]  = rowPlan->outStride[1];
            colPlan->inStride.push_back(rowPlan->outStride[0]);
            colPlan->iDist         = rowPlan->oDist;

            //this 2d is decomposed from 3d
            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              colPlan->length.push_back(fftPlan->length[index]);
              colPlan->outStride.push_back(fftPlan->outStride[index]);
              colPlan->inStride.push_back(rowPlan->outStride[index]);
            }

            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planY);
          }
        } else if(fftPlan->opLayout == HCFFT_REAL) {
          length0 = fftPlan->length[0];
          length1 = fftPlan->length[1];
          size_t Nt = (1 + length0 / 2);

          if (fftPlan->tmpBufSize == 0) {
            fftPlan->tmpBufSize = Nt * length1 * fftPlan->batchSize * fftPlan->ElementSize();

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              fftPlan->tmpBufSize *= fftPlan->length[index];
            }
          }

          if ((fftPlan->tmpBufSizeC2R == 0) && (fftPlan->location == HCFFT_OUTOFPLACE) && (fftPlan->length.size() == 2)) {
            fftPlan->tmpBufSizeC2R = fftPlan->tmpBufSize;
          }

          if( (fftPlan->inStride[0] == 1) && (fftPlan->outStride[0] == 1) &&
              ( ((fftPlan->outStride[1] == Nt * 2) && (fftPlan->oDist == Nt * 2 * length1) && (fftPlan->location == HCFFT_INPLACE)) ||
                ((fftPlan->outStride[1] == length0) && (fftPlan->oDist == length0 * length1) && (fftPlan->location == HCFFT_OUTOFPLACE)) )
              && (fftPlan->inStride[1] == Nt) && (fftPlan->iDist == Nt * length1) ) {
            // create first transpose plan
            //Transpose
            // input --> tmp
            size_t transLengths[2] = { length0, length1 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTY, HCFFT_2D, transLengths);
            FFTPlan* trans1Plan = NULL;
            lockRAII* trans1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTY, trans1Plan, trans1Lock );
            trans1Plan->transflag = true;
            transLengths[0] = Nt;
            hcfftSetPlanLength( fftPlan->planTY, HCFFT_2D, transLengths );

            switch(fftPlan->ipLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans1Plan->location     = HCFFT_OUTOFPLACE;
            trans1Plan->precision     = fftPlan->precision;
            trans1Plan->tmpBufSize    = 0;
            trans1Plan->batchSize     = fftPlan->batchSize;
            trans1Plan->envelope    = fftPlan->envelope;
            trans1Plan->forwardScale  = 1.0f;
            trans1Plan->backwardScale = 1.0f;
            trans1Plan->inStride[0]   = 1;
            trans1Plan->inStride[1]   = Nt;
            trans1Plan->outStride[0]  = 1;
            trans1Plan->outStride[1]  = length1;
            trans1Plan->iDist         = fftPlan->iDist;
            trans1Plan->oDist   = Nt * length1;
            trans1Plan->transOutHorizontal = true;
            trans1Plan->gen           = Transpose_GCN;

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              trans1Plan->length.push_back(fftPlan->length[index]);
              trans1Plan->inStride.push_back(fftPlan->inStride[index]);
              trans1Plan->outStride.push_back(trans1Plan->oDist);
              trans1Plan->oDist *= fftPlan->length[index];
            }

            trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans1Plan->originalLength  = fftPlan->originalLength;
            trans1Plan->acc  = fftPlan->acc;
            trans1Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTY);
            // create col plan
            // complex to complex
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &fftPlan->length[ 1 ] );
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planY, colPlan, colLock );
            colPlan->length.push_back(Nt);
            colPlan->inStride[0]  = 1;
            colPlan->inStride.push_back(length1);
            colPlan->iDist         = trans1Plan->oDist;
            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->location = HCFFT_INPLACE;
            colPlan->ipLayout = HCFFT_COMPLEX_INTERLEAVED;
            colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
            colPlan->outStride[0]  = colPlan->inStride[0];
            colPlan->outStride.push_back(colPlan->inStride[1]);
            colPlan->oDist         = colPlan->iDist;

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              colPlan->length.push_back(fftPlan->length[index]);
              colPlan->inStride.push_back(trans1Plan->outStride[index]);
              colPlan->outStride.push_back(trans1Plan->outStride[index]);
            }

            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = 1.0f;
            colPlan->backwardScale = 1.0f;
            colPlan->tmpBufSize    = 0;
            colPlan->gen      = fftPlan->gen;
            colPlan->envelope   = fftPlan->envelope;
            colPlan->batchSize = fftPlan->batchSize;
            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planY);
            // create second transpose plan
            //Transpose
            //tmp --> output
            size_t trans2Lengths[2] = { length1, length0 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, trans2Lengths );
            FFTPlan* trans2Plan = NULL;
            lockRAII* trans2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTX, trans2Plan, trans2Lock );
            trans2Plan->transflag = true;
            trans2Lengths[1] = Nt;
            hcfftSetPlanLength( fftPlan->planTX, HCFFT_2D, trans2Lengths );
            trans2Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans2Plan->location     = HCFFT_OUTOFPLACE;
            trans2Plan->precision     = fftPlan->precision;
            trans2Plan->tmpBufSize    = 0;
            trans2Plan->batchSize     = fftPlan->batchSize;
            trans2Plan->envelope    = fftPlan->envelope;
            trans2Plan->forwardScale  = 1.0f;
            trans2Plan->backwardScale = 1.0f;
            trans2Plan->inStride[0]   = 1;
            trans2Plan->inStride[1]   = length1;
            trans2Plan->outStride[0]  = 1;
            trans2Plan->outStride[1]  = Nt;
            trans2Plan->iDist         = colPlan->oDist;
            trans2Plan->oDist     = Nt * length1;
            trans2Plan->transflag     = true;
            trans2Plan->gen           = Transpose_GCN;

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              trans2Plan->length.push_back(fftPlan->length[index]);
              trans2Plan->inStride.push_back(colPlan->outStride[index]);
              trans2Plan->outStride.push_back(trans2Plan->oDist);
              trans2Plan->oDist *= fftPlan->length[index];
            }

            trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans2Plan->originalLength  = fftPlan->originalLength;
            trans2Plan->acc  = fftPlan->acc;
            trans2Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTX);
            // create row plan
            // hermitian to real
            //create row plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &fftPlan->length[ 0 ]);
            FFTPlan* rowPlan  = NULL;
            lockRAII* rowLock = NULL;
            fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
            rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            rowPlan->opLayout  = fftPlan->opLayout;
            rowPlan->ipLayout   = HCFFT_HERMITIAN_INTERLEAVED;
            rowPlan->length.push_back(length1);
            rowPlan->outStride[0]  = fftPlan->outStride[0];
            rowPlan->outStride.push_back(fftPlan->outStride[1]);
            rowPlan->oDist         = fftPlan->oDist;
            rowPlan->inStride[0]  = trans2Plan->outStride[0];
            rowPlan->inStride.push_back(trans2Plan->outStride[1]);
            rowPlan->iDist         = trans2Plan->oDist;

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              rowPlan->length.push_back(fftPlan->length[index]);
              rowPlan->inStride.push_back(trans2Plan->outStride[index]);
              rowPlan->outStride.push_back(fftPlan->outStride[index]);
            }

            if (fftPlan->location == HCFFT_INPLACE) {
              rowPlan->location     = HCFFT_INPLACE;
            } else {
              rowPlan->location     = HCFFT_OUTOFPLACE;
            }

            rowPlan->precision     = fftPlan->precision;
            rowPlan->forwardScale  = fftPlan->forwardScale;
            rowPlan->backwardScale = fftPlan->backwardScale;
            rowPlan->tmpBufSize    = 0;
            rowPlan->gen      = fftPlan->gen;
            rowPlan->envelope   = fftPlan->envelope;
            rowPlan->batchSize    = fftPlan->batchSize;
            rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            rowPlan->originalLength  = fftPlan->originalLength;
            rowPlan->acc  = fftPlan->acc;
            rowPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planX);
          } else {
            // create col plan
            // complex to complex
            hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &fftPlan->length[ 1 ]);
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planY, colPlan, colLock );

            switch(fftPlan->ipLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  colPlan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  colPlan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            colPlan->length.push_back(Nt);
            colPlan->inStride[0]  = fftPlan->inStride[1];
            colPlan->inStride.push_back(fftPlan->inStride[0]);
            colPlan->iDist         = fftPlan->iDist;

            if (fftPlan->location == HCFFT_INPLACE) {
              colPlan->location = HCFFT_INPLACE;
            } else {
              if(fftPlan->length.size() > 2) {
                colPlan->location = HCFFT_INPLACE;
              } else {
                colPlan->location = HCFFT_OUTOFPLACE;
              }
            }

            if(colPlan->location == HCFFT_INPLACE) {
              colPlan->outStride[0]  = colPlan->inStride[0];
              colPlan->outStride.push_back(colPlan->inStride[1]);
              colPlan->oDist         = colPlan->iDist;

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                colPlan->length.push_back(fftPlan->length[index]);
                colPlan->inStride.push_back(fftPlan->inStride[index]);
                colPlan->outStride.push_back(fftPlan->inStride[index]);
              }
            } else {
              colPlan->outStride[0]  = Nt;
              colPlan->outStride.push_back(1);
              colPlan->oDist         = Nt * length1;

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                colPlan->length.push_back(fftPlan->length[index]);
                colPlan->inStride.push_back(fftPlan->inStride[index]);
                colPlan->outStride.push_back(colPlan->oDist);
                colPlan->oDist *= fftPlan->length[index];
              }
            }

            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = 1.0f;
            colPlan->backwardScale = 1.0f;
            colPlan->tmpBufSize    = 0;
            colPlan->gen    = fftPlan->gen;
            colPlan->envelope = fftPlan->envelope;
            colPlan->batchSize = fftPlan->batchSize;
            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planY);
            // create row plan
            // hermitian to real
            //create row plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &fftPlan->length[ 0 ]);
            FFTPlan* rowPlan  = NULL;
            lockRAII* rowLock = NULL;
            fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
            rowPlan->opLayout  = fftPlan->opLayout;
            rowPlan->ipLayout   = HCFFT_HERMITIAN_INTERLEAVED;
            rowPlan->length.push_back(length1);
            rowPlan->outStride[0]  = fftPlan->outStride[0];
            rowPlan->outStride.push_back(fftPlan->outStride[1]);
            rowPlan->oDist         = fftPlan->oDist;

            if (fftPlan->location == HCFFT_INPLACE) {
              rowPlan->location     = HCFFT_INPLACE;
              rowPlan->inStride[0]  = colPlan->outStride[1];
              rowPlan->inStride.push_back(colPlan->outStride[0]);
              rowPlan->iDist         = colPlan->oDist;

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                rowPlan->length.push_back(fftPlan->length[index]);
                rowPlan->inStride.push_back(colPlan->outStride[index]);
                rowPlan->outStride.push_back(fftPlan->outStride[index]);
              }
            } else {
              rowPlan->location     = HCFFT_OUTOFPLACE;
              rowPlan->inStride[0]   = 1;
              rowPlan->inStride.push_back(Nt);
              rowPlan->iDist         = Nt * length1;

              for (size_t index = 2; index < fftPlan->length.size(); index++) {
                rowPlan->length.push_back(fftPlan->length[index]);
                rowPlan->outStride.push_back(fftPlan->outStride[index]);
                rowPlan->inStride.push_back(rowPlan->iDist);
                rowPlan->iDist *= fftPlan->length[index];
              }
            }

            rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            rowPlan->precision     = fftPlan->precision;
            rowPlan->forwardScale  = fftPlan->forwardScale;
            rowPlan->backwardScale = fftPlan->backwardScale;
            rowPlan->tmpBufSize    = 0;
            rowPlan->gen      = fftPlan->gen;
            rowPlan->envelope   = fftPlan->envelope;
            rowPlan->batchSize    = fftPlan->batchSize;
            rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            rowPlan->originalLength  = fftPlan->originalLength;
            rowPlan->acc  = fftPlan->acc;
            rowPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planX);
          }
        } else {
          if (fftPlan->tmpBufSize == 0 && fftPlan->length.size() <= 2) {
            fftPlan->tmpBufSize = length0 * length1 *
                                  fftPlan->batchSize * fftPlan->ElementSize();
          }

          //create row plan
          hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_1D, &fftPlan->length[ 0 ]);
          FFTPlan* rowPlan  = NULL;
          lockRAII* rowLock = NULL;
          fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
          rowPlan->ipLayout   = fftPlan->ipLayout;

          if (fftPlan->large2D || fftPlan->length.size() > 2) {
            rowPlan->opLayout  = fftPlan->opLayout;
            rowPlan->location     = fftPlan->location;
            rowPlan->outStride[0]  = fftPlan->outStride[0];
            rowPlan->outStride.push_back(fftPlan->outStride[1]);
            rowPlan->oDist         = fftPlan->oDist;
          } else {
            rowPlan->opLayout  = HCFFT_COMPLEX_INTERLEAVED;
            rowPlan->location     = HCFFT_OUTOFPLACE;
            rowPlan->outStride[0]  = length1;//1;
            rowPlan->outStride.push_back(1);//length0);
            rowPlan->oDist         = length0 * length1;
          }

          rowPlan->precision     = fftPlan->precision;
          rowPlan->forwardScale  = 1.0f;
          rowPlan->backwardScale = 1.0f;
          rowPlan->tmpBufSize    = fftPlan->tmpBufSize;
          rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          rowPlan->gen    = fftPlan->gen;
          rowPlan->envelope = fftPlan->envelope;
          // This is the row fft, the first elements distance between the first two FFTs is the distance of the first elements
          // of the first two rows in the original buffer.
          rowPlan->batchSize    = fftPlan->batchSize;
          rowPlan->inStride[0]  = fftPlan->inStride[0];
          //pass length and other info to kernel, so the kernel knows this is decomposed from higher dimension
          rowPlan->length.push_back(fftPlan->length[1]);
          rowPlan->inStride.push_back(fftPlan->inStride[1]);

          //this 2d is decomposed from 3d
          if (fftPlan->length.size() > 2) {
            rowPlan->length.push_back(fftPlan->length[2]);
            rowPlan->inStride.push_back(fftPlan->inStride[2]);
            rowPlan->outStride.push_back(fftPlan->outStride[2]);
          }

          rowPlan->iDist    = fftPlan->iDist;
          rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          rowPlan->originalLength  = fftPlan->originalLength;
          rowPlan->acc  = fftPlan->acc;
          rowPlan->exist  = fftPlan->exist;
          hcfftBakePlanInternal(fftPlan->planX);
          //create col plan
          hcfftCreateDefaultPlanInternal( &fftPlan->planY, HCFFT_1D, &fftPlan->length[ 1 ] );
          FFTPlan* colPlan  = NULL;
          lockRAII* colLock = NULL;
          fftRepo.getPlan( fftPlan->planY, colPlan, colLock );

          if (fftPlan->large2D || fftPlan->length.size() > 2) {
            colPlan->ipLayout   = fftPlan->opLayout;
            colPlan->location     = HCFFT_INPLACE;
            colPlan->inStride[0]   = fftPlan->outStride[1];
            colPlan->inStride.push_back(fftPlan->outStride[0]);
            colPlan->iDist         = fftPlan->oDist;
          } else {
            colPlan->ipLayout   = HCFFT_COMPLEX_INTERLEAVED;
            colPlan->location     = HCFFT_OUTOFPLACE;
            colPlan->inStride[0]   = 1;//length0;
            colPlan->inStride.push_back(length1);//1);
            colPlan->iDist         = length0 * length1;
          }

          colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          colPlan->opLayout  = fftPlan->opLayout;
          colPlan->precision     = fftPlan->precision;
          colPlan->forwardScale  = fftPlan->forwardScale;
          colPlan->backwardScale = fftPlan->backwardScale;
          colPlan->tmpBufSize    = fftPlan->tmpBufSize;
          colPlan->gen  = fftPlan->gen;
          colPlan->envelope = fftPlan->envelope;
          // This is a column FFT, the first elements distance between each FFT is the distance of the first two
          // elements in the original buffer. Like a transpose of the matrix
          colPlan->batchSize = fftPlan->batchSize;
          colPlan->outStride[0] = fftPlan->outStride[1];
          //pass length and other info to kernel, so the kernel knows this is decomposed from higher dimension
          colPlan->length.push_back(fftPlan->length[0]);
          colPlan->outStride.push_back(fftPlan->outStride[0]);
          colPlan->oDist    = fftPlan->oDist;

          //this 2d is decomposed from 3d
          if (fftPlan->length.size() > 2) {
            //assert(fftPlan->large2D);
            colPlan->length.push_back(fftPlan->length[2]);
            colPlan->inStride.push_back(fftPlan->outStride[2]);
            colPlan->outStride.push_back(fftPlan->outStride[2]);
          }

          colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          colPlan->originalLength  = fftPlan->originalLength;
          colPlan->acc  = fftPlan->acc;
          colPlan->exist  = fftPlan->exist;
          hcfftBakePlanInternal(fftPlan->planY);
        }

        fftPlan->baked = true;
        return  HCFFT_SUCCEEDS;
      }

    case HCFFT_3D: {
        if(fftPlan->ipLayout == HCFFT_REAL) {
          size_t length0 = fftPlan->length[ 0 ];
          size_t length1 = fftPlan->length[ 1 ];
          size_t length2 = fftPlan->length[ 2 ];
          size_t Nt = (1 + length0 / 2);
          //create 2D xy plan
          size_t hcLengths[] = { length0, length1, 0 };
          hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_2D, hcLengths );
          FFTPlan* xyPlan = NULL;
          lockRAII* rowLock = NULL;
          fftRepo.getPlan( fftPlan->planX, xyPlan, rowLock );
          xyPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          xyPlan->ipLayout   = fftPlan->ipLayout;
          xyPlan->opLayout  = fftPlan->opLayout;
          xyPlan->location     = fftPlan->location;
          xyPlan->precision     = fftPlan->precision;
          xyPlan->forwardScale  = 1.0f;
          xyPlan->backwardScale = 1.0f;
          xyPlan->tmpBufSize    = fftPlan->tmpBufSize;
          xyPlan->gen      = fftPlan->gen;
          xyPlan->envelope       = fftPlan->envelope;
          // This is the xy fft, the first elements distance between the first two FFTs is the distance of the first elements
          // of the first two rows in the original buffer.
          xyPlan->batchSize    = fftPlan->batchSize;
          xyPlan->inStride[0]  = fftPlan->inStride[0];
          xyPlan->inStride[1]  = fftPlan->inStride[1];
          xyPlan->outStride[0] = fftPlan->outStride[0];
          xyPlan->outStride[1] = fftPlan->outStride[1];
          //pass length and other info to kernel, so the kernel knows this is decomposed from higher dimension
          xyPlan->length.push_back(fftPlan->length[2]);
          xyPlan->inStride.push_back(fftPlan->inStride[2]);
          xyPlan->outStride.push_back(fftPlan->outStride[2]);
          xyPlan->iDist    = fftPlan->iDist;
          xyPlan->oDist    = fftPlan->oDist;

          //this 3d is decomposed from 4d
          for (size_t index = 3; index < fftPlan->length.size(); index++) {
            xyPlan->length.push_back(fftPlan->length[index]);
            xyPlan->inStride.push_back(fftPlan->inStride[index]);
            xyPlan->outStride.push_back(fftPlan->outStride[index]);
          }

          xyPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          xyPlan->originalLength  = fftPlan->originalLength;
          xyPlan->acc  = fftPlan->acc;
          xyPlan->exist  = fftPlan->exist;
          hcfftBakePlanInternal(fftPlan->planX);

          if( (xyPlan->inStride[0] == 1) && (xyPlan->outStride[0] == 1) &&
              (xyPlan->outStride[2] == Nt * length1) &&
              ( ((xyPlan->inStride[2] == Nt * 2 * length1) && (xyPlan->location == HCFFT_INPLACE)) ||
                ((xyPlan->inStride[2] == length0 * length1) && (xyPlan->location == HCFFT_OUTOFPLACE)) ) ) {
            if (fftPlan->tmpBufSize == 0) {
              fftPlan->tmpBufSize = Nt * length1 * length2 * fftPlan->batchSize * fftPlan->ElementSize();

              for (size_t index = 3; index < fftPlan->length.size(); index++) {
                fftPlan->tmpBufSize *= fftPlan->length[index];
              }
            }

            // create first transpose plan
            //Transpose
            // output --> tmp
            size_t transLengths[2] = { length0 * length1, length2 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, transLengths );
            FFTPlan* trans1Plan = NULL;
            lockRAII* trans1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTX, trans1Plan, trans1Lock );
            trans1Plan->transflag = true;
            transLengths[0] = Nt * length1;
            hcfftSetPlanLength( fftPlan->planTX, HCFFT_2D, transLengths );

            switch(fftPlan->opLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans1Plan->location     = HCFFT_OUTOFPLACE;
            trans1Plan->precision     = fftPlan->precision;
            trans1Plan->tmpBufSize    = 0;
            trans1Plan->batchSize     = fftPlan->batchSize;
            trans1Plan->envelope    = fftPlan->envelope;
            trans1Plan->forwardScale  = 1.0f;
            trans1Plan->backwardScale = 1.0f;
            trans1Plan->inStride[0]   = 1;
            trans1Plan->inStride[1]   = Nt * length1;
            trans1Plan->outStride[0]  = 1;
            trans1Plan->outStride[1]  = length2;
            trans1Plan->iDist         = xyPlan->oDist;
            trans1Plan->oDist     = Nt * length1 * length2;
            trans1Plan->transOutHorizontal = true;
            trans1Plan->gen           = Transpose_GCN;

            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              trans1Plan->length.push_back(fftPlan->length[index]);
              trans1Plan->inStride.push_back(xyPlan->outStride[index]);
              trans1Plan->outStride.push_back(trans1Plan->oDist);
              trans1Plan->oDist *= fftPlan->length[index];
            }

            trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans1Plan->originalLength  = fftPlan->originalLength;
            trans1Plan->acc  = fftPlan->acc;
            trans1Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTX);
            // Create column plan as a row plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planZ, HCFFT_1D, &fftPlan->length[2] );
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planZ, colPlan, colLock );
            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->opLayout  = trans1Plan->opLayout;
            colPlan->ipLayout   = trans1Plan->opLayout;
            colPlan->location     = HCFFT_INPLACE;
            colPlan->length.push_back(Nt * length1);
            colPlan->inStride[0]  = 1;
            colPlan->inStride.push_back(length2);
            colPlan->iDist         = Nt * length1 * length2;
            colPlan->outStride[0]  = 1;
            colPlan->outStride.push_back(length2);
            colPlan->oDist         = Nt * length1 * length2;
            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = fftPlan->forwardScale;
            colPlan->backwardScale = fftPlan->backwardScale;
            colPlan->tmpBufSize    = 0;
            colPlan->gen      = fftPlan->gen;
            colPlan->envelope   = fftPlan->envelope;
            colPlan->batchSize    = fftPlan->batchSize;

            //this 2d is decomposed from 3d
            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              colPlan->length.push_back(fftPlan->length[index]);
              colPlan->inStride.push_back(colPlan->iDist);
              colPlan->outStride.push_back(colPlan->oDist);
              colPlan->iDist *= fftPlan->length[index];
              colPlan->oDist *= fftPlan->length[index];
            }

            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planZ);

            if (fftPlan->transposeType == HCFFT_TRANSPOSED) {
              fftPlan->baked = true;
              return  HCFFT_SUCCEEDS;
            }

            // create second transpose plan
            //Transpose
            //output --> tmp
            size_t trans2Lengths[2] = { length2, length0 * length1 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTY, HCFFT_2D, trans2Lengths );
            FFTPlan* trans2Plan = NULL;
            lockRAII* trans2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTY, trans2Plan, trans2Lock );
            trans2Plan->transflag = true;
            trans2Lengths[1] = Nt * length1;
            hcfftSetPlanLength( fftPlan->planTY, HCFFT_2D, trans2Lengths );

            switch(fftPlan->opLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  trans2Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  trans2Plan->opLayout = HCFFT_COMPLEX_PLANAR;
                  trans2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              default:
                assert(false);
            }

            trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans2Plan->location     = HCFFT_OUTOFPLACE;
            trans2Plan->precision     = fftPlan->precision;
            trans2Plan->tmpBufSize    = 0;
            trans2Plan->batchSize     = fftPlan->batchSize;
            trans2Plan->envelope    = fftPlan->envelope;
            trans2Plan->forwardScale  = 1.0f;
            trans2Plan->backwardScale = 1.0f;
            trans2Plan->inStride[0]   = 1;
            trans2Plan->inStride[1]   = length2;
            trans2Plan->outStride[0]  = 1;
            trans2Plan->outStride[1]  = Nt * length1;
            trans2Plan->iDist         = Nt * length1 * length2;
            trans2Plan->oDist     = fftPlan->oDist;
            trans2Plan->gen           = Transpose_GCN;
            trans2Plan->transflag     = true;

            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              trans2Plan->length.push_back(fftPlan->length[index]);
              trans2Plan->inStride.push_back(trans2Plan->iDist);
              trans2Plan->iDist *= fftPlan->length[index];
              trans2Plan->outStride.push_back(fftPlan->outStride[index]);
            }

            trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans2Plan->originalLength  = fftPlan->originalLength;
            trans2Plan->acc  = fftPlan->acc;
            trans2Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTY);
          } else {
            hcLengths[0] = fftPlan->length[ 2 ];
            hcLengths[1] = hcLengths[2] = 0;
            //create 1D col plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planZ, HCFFT_1D, hcLengths );
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planZ, colPlan, colLock );

            switch(fftPlan->opLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  colPlan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  colPlan->opLayout = HCFFT_COMPLEX_PLANAR;
                  colPlan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->location     = HCFFT_INPLACE;
            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = fftPlan->forwardScale;
            colPlan->backwardScale = fftPlan->backwardScale;
            colPlan->tmpBufSize    = fftPlan->tmpBufSize;
            colPlan->gen       = fftPlan->gen;
            colPlan->envelope      = fftPlan->envelope;
            // This is a column FFT, the first elements distance between each FFT is the distance of the first two
            // elements in the original buffer. Like a transpose of the matrix
            colPlan->batchSize = fftPlan->batchSize;
            colPlan->inStride[0] = fftPlan->outStride[2];
            colPlan->outStride[0] = fftPlan->outStride[2];
            //pass length and other info to kernel, so the kernel knows this is decomposed from higher dimension
            colPlan->length.push_back(1 + fftPlan->length[0] / 2);
            colPlan->length.push_back(fftPlan->length[1]);
            colPlan->inStride.push_back(fftPlan->outStride[0]);
            colPlan->inStride.push_back(fftPlan->outStride[1]);
            colPlan->outStride.push_back(fftPlan->outStride[0]);
            colPlan->outStride.push_back(fftPlan->outStride[1]);
            colPlan->iDist    = fftPlan->oDist;
            colPlan->oDist    = fftPlan->oDist;

            //this 3d is decomposed from 4d
            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              colPlan->length.push_back(fftPlan->length[index]);
              colPlan->inStride.push_back(xyPlan->outStride[index]);
              colPlan->outStride.push_back(fftPlan->outStride[index]);
            }

            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planZ);
          }
        } else if(fftPlan->opLayout == HCFFT_REAL) {
          size_t length0 = fftPlan->length[ 0 ];
          size_t length1 = fftPlan->length[ 1 ];
          size_t length2 = fftPlan->length[ 2 ];
          size_t Nt = (1 + length0 / 2);

          if (fftPlan->tmpBufSize == 0) {
            fftPlan->tmpBufSize = Nt * length1 * length2 * fftPlan->batchSize * fftPlan->ElementSize();

            for (size_t index = 2; index < fftPlan->length.size(); index++) {
              fftPlan->tmpBufSize *= fftPlan->length[index];
            }
          }

          if ((fftPlan->tmpBufSizeC2R == 0) && (fftPlan->location == HCFFT_OUTOFPLACE)) {
            fftPlan->tmpBufSizeC2R = fftPlan->tmpBufSize;
          }

          if( (fftPlan->inStride[0] == 1) && (fftPlan->outStride[0] == 1) &&
              ( ((fftPlan->outStride[2] == Nt * 2 * length1) && (fftPlan->oDist == Nt * 2 * length1 * length2) && (fftPlan->location == HCFFT_INPLACE)) ||
                ((fftPlan->outStride[2] == length0 * length1) && (fftPlan->oDist == length0 * length1 * length2) && (fftPlan->location == HCFFT_OUTOFPLACE)) )
              && (fftPlan->inStride[2] == Nt * length1) && (fftPlan->iDist == Nt * length1 * length2)) {
            // create first transpose plan
            //Transpose
            // input --> tmp
            size_t transLengths[2] = { length0 * length1, length2 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTZ, HCFFT_2D, transLengths );
            FFTPlan* trans1Plan = NULL;
            lockRAII* trans1Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTZ, trans1Plan, trans1Lock );
            trans1Plan->transflag = true;
            transLengths[0] = Nt * length1;
            hcfftSetPlanLength( fftPlan->planTZ, HCFFT_2D, transLengths );

            switch(fftPlan->ipLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  trans1Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  trans1Plan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            trans1Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans1Plan->location     = HCFFT_OUTOFPLACE;
            trans1Plan->precision     = fftPlan->precision;
            trans1Plan->tmpBufSize    = 0;
            trans1Plan->batchSize     = fftPlan->batchSize;
            trans1Plan->envelope    = fftPlan->envelope;
            trans1Plan->forwardScale  = 1.0f;
            trans1Plan->backwardScale = 1.0f;
            trans1Plan->inStride[0]   = 1;
            trans1Plan->inStride[1]   = Nt * length1;
            trans1Plan->outStride[0]  = 1;
            trans1Plan->outStride[1]  = length2;
            trans1Plan->iDist         = fftPlan->iDist;
            trans1Plan->oDist   = Nt * length1 * length2;
            trans1Plan->transOutHorizontal = true;
            trans1Plan->gen           = Transpose_GCN;

            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              trans1Plan->length.push_back(fftPlan->length[index]);
              trans1Plan->inStride.push_back(fftPlan->inStride[index]);
              trans1Plan->outStride.push_back(trans1Plan->oDist);
              trans1Plan->oDist *= fftPlan->length[index];
            }

            trans1Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans1Plan->originalLength  = fftPlan->originalLength;
            trans1Plan->acc  = fftPlan->acc;
            trans1Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTZ);
            // create col plan
            // complex to complex
            hcfftCreateDefaultPlanInternal( &fftPlan->planZ, HCFFT_1D, &fftPlan->length[ 2 ] );
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planZ, colPlan, colLock );
            colPlan->length.push_back(Nt * length1);
            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->inStride[0]  = 1;
            colPlan->inStride.push_back(length2);
            colPlan->iDist        = trans1Plan->oDist;
            colPlan->location = HCFFT_INPLACE;
            colPlan->ipLayout = HCFFT_COMPLEX_INTERLEAVED;
            colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
            colPlan->outStride[0]  = colPlan->inStride[0];
            colPlan->outStride.push_back(colPlan->inStride[1]);
            colPlan->oDist         = colPlan->iDist;

            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              colPlan->length.push_back(fftPlan->length[index]);
              colPlan->inStride.push_back(trans1Plan->outStride[index - 1]);
              colPlan->outStride.push_back(trans1Plan->outStride[index - 1]);
            }

            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = 1.0f;
            colPlan->backwardScale = 1.0f;
            colPlan->tmpBufSize    = 0;
            colPlan->gen      = fftPlan->gen;
            colPlan->envelope   = fftPlan->envelope;
            colPlan->batchSize = fftPlan->batchSize;
            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planZ);
            // create second transpose plan
            //Transpose
            //tmp --> output
            size_t trans2Lengths[2] = { length2, length0 * length1 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planTX, HCFFT_2D, trans2Lengths );
            FFTPlan* trans2Plan = NULL;
            lockRAII* trans2Lock  = NULL;
            fftRepo.getPlan( fftPlan->planTX, trans2Plan, trans2Lock );
            trans2Plan->transflag = true;
            trans2Lengths[1] = Nt * length1;
            hcfftSetPlanLength( fftPlan->planTX, HCFFT_2D, trans2Lengths );
            trans2Plan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
            trans2Plan->plHandleOrigin  = fftPlan->plHandleOrigin;
            trans2Plan->location     = HCFFT_OUTOFPLACE;
            trans2Plan->precision     = fftPlan->precision;
            trans2Plan->tmpBufSize    = 0;
            trans2Plan->batchSize     = fftPlan->batchSize;
            trans2Plan->envelope    = fftPlan->envelope;
            trans2Plan->forwardScale  = 1.0f;
            trans2Plan->backwardScale = 1.0f;
            trans2Plan->inStride[0]   = 1;
            trans2Plan->inStride[1]   = length2;
            trans2Plan->outStride[0]  = 1;
            trans2Plan->outStride[1]  = Nt * length1;
            trans2Plan->iDist         = colPlan->oDist;
            trans2Plan->oDist   = Nt * length1 * length2;
            trans2Plan->gen           = Transpose_GCN;
            trans2Plan->transflag     = true;

            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              trans2Plan->length.push_back(fftPlan->length[index]);
              trans2Plan->inStride.push_back(colPlan->outStride[index - 1]);
              trans2Plan->outStride.push_back(trans2Plan->oDist);
              trans2Plan->oDist *= fftPlan->length[index];
            }

            trans2Plan->hcfftlibtype  = fftPlan->hcfftlibtype;
            trans2Plan->originalLength  = fftPlan->originalLength;
            trans2Plan->acc  = fftPlan->acc;
            trans2Plan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planTX);
            // create row plan
            // hermitian to real
            //create 2D xy plan
            size_t hcLengths[] = { length0, length1, 0 };
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_2D, hcLengths );
            FFTPlan* rowPlan  = NULL;
            lockRAII* rowLock = NULL;
            fftRepo.getPlan( fftPlan->planX, rowPlan, rowLock );
            rowPlan->opLayout  = fftPlan->opLayout;
            rowPlan->ipLayout   = HCFFT_HERMITIAN_INTERLEAVED;
            rowPlan->length.push_back(length2);
            rowPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            rowPlan->outStride[0]  = fftPlan->outStride[0];
            rowPlan->outStride[1]  = fftPlan->outStride[1];
            rowPlan->outStride.push_back(fftPlan->outStride[2]);
            rowPlan->oDist         = fftPlan->oDist;
            rowPlan->inStride[0]  = trans2Plan->outStride[0];
            rowPlan->inStride[1]  = Nt;
            rowPlan->inStride.push_back(Nt * length1);
            rowPlan->iDist         = trans2Plan->oDist;

            for (size_t index = 3; index < fftPlan->length.size(); index++) {
              rowPlan->length.push_back(fftPlan->length[index]);
              rowPlan->inStride.push_back(trans2Plan->outStride[index - 1]);
              rowPlan->outStride.push_back(fftPlan->outStride[index]);
            }

            if (fftPlan->location == HCFFT_INPLACE) {
              rowPlan->location     = HCFFT_INPLACE;
            } else {
              rowPlan->location     = HCFFT_OUTOFPLACE;
            }

            rowPlan->precision     = fftPlan->precision;
            rowPlan->forwardScale  = fftPlan->forwardScale;
            rowPlan->backwardScale = fftPlan->backwardScale;
            rowPlan->tmpBufSize    = 0;
            rowPlan->gen      = fftPlan->gen;
            rowPlan->envelope   = fftPlan->envelope;
            rowPlan->batchSize    = fftPlan->batchSize;
            rowPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            rowPlan->originalLength  = fftPlan->originalLength;
            rowPlan->acc  = fftPlan->acc;
            rowPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planX);
          } else {
            size_t hcLengths[] = { 1, 0, 0 };
            hcLengths[0] = fftPlan->length[ 2 ];
            //create 1D col plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planZ, HCFFT_1D, hcLengths );
            FFTPlan* colPlan  = NULL;
            lockRAII* colLock = NULL;
            fftRepo.getPlan( fftPlan->planZ, colPlan, colLock );

            switch(fftPlan->ipLayout) {
              case HCFFT_HERMITIAN_INTERLEAVED: {
                  colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  colPlan->ipLayout  = HCFFT_COMPLEX_INTERLEAVED;
                }
                break;

              case HCFFT_HERMITIAN_PLANAR: {
                  colPlan->opLayout = HCFFT_COMPLEX_INTERLEAVED;
                  colPlan->ipLayout  = HCFFT_COMPLEX_PLANAR;
                }
                break;

              default:
                assert(false);
            }

            colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            colPlan->length.push_back(Nt);
            colPlan->length.push_back(length1);
            colPlan->inStride[0]  = fftPlan->inStride[2];
            colPlan->inStride.push_back(fftPlan->inStride[0]);
            colPlan->inStride.push_back(fftPlan->inStride[1]);
            colPlan->iDist         = fftPlan->iDist;

            if (fftPlan->location == HCFFT_INPLACE) {
              colPlan->location = HCFFT_INPLACE;
              colPlan->outStride[0]  = colPlan->inStride[0];
              colPlan->outStride.push_back(colPlan->inStride[1]);
              colPlan->outStride.push_back(colPlan->inStride[2]);
              colPlan->oDist         = colPlan->iDist;

              for (size_t index = 3; index < fftPlan->length.size(); index++) {
                colPlan->length.push_back(fftPlan->length[index]);
                colPlan->inStride.push_back(fftPlan->inStride[index]);
                colPlan->outStride.push_back(fftPlan->inStride[index]);
              }
            } else {
              colPlan->location = HCFFT_OUTOFPLACE;
              colPlan->outStride[0]  = Nt * length1;
              colPlan->outStride.push_back(1);
              colPlan->outStride.push_back(Nt);
              colPlan->oDist         = Nt * length1 * length2;

              for (size_t index = 3; index < fftPlan->length.size(); index++) {
                colPlan->length.push_back(fftPlan->length[index]);
                colPlan->inStride.push_back(fftPlan->inStride[index]);
                colPlan->outStride.push_back(colPlan->oDist);
                colPlan->oDist *= fftPlan->length[index];
              }
            }

            colPlan->precision     = fftPlan->precision;
            colPlan->forwardScale  = 1.0f;
            colPlan->backwardScale = 1.0f;
            colPlan->tmpBufSize    = 0;
            colPlan->gen       = fftPlan->gen;
            colPlan->envelope    = fftPlan->envelope;
            colPlan->batchSize = fftPlan->batchSize;
            colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            colPlan->originalLength  = fftPlan->originalLength;
            colPlan->acc  = fftPlan->acc;
            colPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planZ);
            hcLengths[0] = fftPlan->length[ 0 ];
            hcLengths[1] = fftPlan->length[ 1 ];
            //create 2D xy plan
            hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_2D, hcLengths );
            FFTPlan* xyPlan = NULL;
            lockRAII* rowLock = NULL;
            fftRepo.getPlan( fftPlan->planX, xyPlan, rowLock );
            xyPlan->ipLayout   = HCFFT_HERMITIAN_INTERLEAVED;
            xyPlan->opLayout  = fftPlan->opLayout;
            xyPlan->length.push_back(length2);
            xyPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
            xyPlan->outStride[0]  = fftPlan->outStride[0];
            xyPlan->outStride[1]  = fftPlan->outStride[1];
            xyPlan->outStride.push_back(fftPlan->outStride[2]);
            xyPlan->oDist         = fftPlan->oDist;

            if (fftPlan->location == HCFFT_INPLACE) {
              xyPlan->location     = HCFFT_INPLACE;
              xyPlan->inStride[0]  = colPlan->outStride[1];
              xyPlan->inStride[1]  = colPlan->outStride[2];
              xyPlan->inStride.push_back(colPlan->outStride[0]);
              xyPlan->iDist         = colPlan->oDist;

              for (size_t index = 3; index < fftPlan->length.size(); index++) {
                xyPlan->length.push_back(fftPlan->length[index]);
                xyPlan->inStride.push_back(colPlan->outStride[index]);
                xyPlan->outStride.push_back(fftPlan->outStride[index]);
              }
            } else {
              xyPlan->location     = HCFFT_OUTOFPLACE;
              xyPlan->inStride[0]   = 1;
              xyPlan->inStride[1]   = Nt;
              xyPlan->inStride.push_back(Nt * length1);
              xyPlan->iDist         = Nt * length1 * length2;

              for (size_t index = 3; index < fftPlan->length.size(); index++) {
                xyPlan->length.push_back(fftPlan->length[index]);
                xyPlan->outStride.push_back(fftPlan->outStride[index]);
                xyPlan->inStride.push_back(xyPlan->iDist);
                xyPlan->iDist *= fftPlan->length[index];
              }
            }

            xyPlan->precision     = fftPlan->precision;
            xyPlan->forwardScale  = fftPlan->forwardScale;
            xyPlan->backwardScale = fftPlan->backwardScale;
            xyPlan->tmpBufSize    = fftPlan->tmpBufSize;
            xyPlan->gen      = fftPlan->gen;
            xyPlan->envelope   = fftPlan->envelope;
            xyPlan->batchSize    = fftPlan->batchSize;
            xyPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
            xyPlan->originalLength  = fftPlan->originalLength;
            xyPlan->acc  = fftPlan->acc;
            xyPlan->exist  = fftPlan->exist;
            hcfftBakePlanInternal(fftPlan->planX);
          }
        } else {
          if (fftPlan->tmpBufSize == 0 && (
                fftPlan->length[0] > Large1DThreshold ||
                fftPlan->length[1] > Large1DThreshold ||
                fftPlan->length[2] > Large1DThreshold
              )) {
            fftPlan->tmpBufSize = fftPlan->length[0] * fftPlan->length[1] * fftPlan->length[2] *
                                  fftPlan->batchSize * fftPlan->ElementSize();
          }

          size_t hcLengths[] = { 1, 1, 0 };
          hcLengths[0] = fftPlan->length[ 0 ];
          hcLengths[1] = fftPlan->length[ 1 ];
          //create 2D xy plan
          hcfftCreateDefaultPlanInternal( &fftPlan->planX, HCFFT_2D, hcLengths );
          FFTPlan* xyPlan = NULL;
          lockRAII* rowLock = NULL;
          fftRepo.getPlan( fftPlan->planX, xyPlan, rowLock );
          xyPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          xyPlan->ipLayout   = fftPlan->ipLayout;
          xyPlan->opLayout  = fftPlan->opLayout;
          xyPlan->location     = fftPlan->location;
          xyPlan->precision     = fftPlan->precision;
          xyPlan->forwardScale  = 1.0f;
          xyPlan->backwardScale = 1.0f;
          xyPlan->tmpBufSize    = fftPlan->tmpBufSize;
          xyPlan->gen      = fftPlan->gen;
          xyPlan->envelope       = fftPlan->envelope;
          // This is the xy fft, the first elements distance between the first two FFTs is the distance of the first elements
          // of the first two rows in the original buffer.
          xyPlan->batchSize    = fftPlan->batchSize;
          xyPlan->inStride[0]  = fftPlan->inStride[0];
          xyPlan->inStride[1]  = fftPlan->inStride[1];
          xyPlan->outStride[0] = fftPlan->outStride[0];
          xyPlan->outStride[1] = fftPlan->outStride[1];
          //pass length and other info to kernel, so the kernel knows this is decomposed from higher dimension
          xyPlan->length.push_back(fftPlan->length[2]);
          xyPlan->inStride.push_back(fftPlan->inStride[2]);
          xyPlan->outStride.push_back(fftPlan->outStride[2]);
          xyPlan->iDist    = fftPlan->iDist;
          xyPlan->oDist    = fftPlan->oDist;
          xyPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          xyPlan->originalLength  = fftPlan->originalLength;
          xyPlan->acc  = fftPlan->acc;
          xyPlan->exist  = fftPlan->exist;
          hcfftBakePlanInternal(fftPlan->planX);
          hcLengths[0] = fftPlan->length[ 2 ];
          hcLengths[1] = hcLengths[2] = 0;
          //create 1D col plan
          hcfftCreateDefaultPlanInternal( &fftPlan->planZ, HCFFT_1D, hcLengths );
          FFTPlan* colPlan  = NULL;
          lockRAII* colLock = NULL;
          fftRepo.getPlan( fftPlan->planZ, colPlan, colLock );
          colPlan->plHandleOrigin  = fftPlan->plHandleOrigin;
          colPlan->ipLayout   = fftPlan->opLayout;
          colPlan->opLayout  = fftPlan->opLayout;
          colPlan->location     = HCFFT_INPLACE;
          colPlan->precision     = fftPlan->precision;
          colPlan->forwardScale  = fftPlan->forwardScale;
          colPlan->backwardScale = fftPlan->backwardScale;
          colPlan->tmpBufSize    = fftPlan->tmpBufSize;
          colPlan->gen         = fftPlan->gen;
          colPlan->envelope      = fftPlan->envelope;
          // This is a column FFT, the first elements distance between each FFT is the distance of the first two
          // elements in the original buffer. Like a transpose of the matrix
          colPlan->batchSize = fftPlan->batchSize;
          colPlan->inStride[0] = fftPlan->outStride[2];
          colPlan->outStride[0] = fftPlan->outStride[2];
          //pass length and other info to kernel, so the kernel knows this is decomposed from higher dimension
          colPlan->length.push_back(fftPlan->length[0]);
          colPlan->length.push_back(fftPlan->length[1]);
          colPlan->inStride.push_back(fftPlan->outStride[0]);
          colPlan->inStride.push_back(fftPlan->outStride[1]);
          colPlan->outStride.push_back(fftPlan->outStride[0]);
          colPlan->outStride.push_back(fftPlan->outStride[1]);
          colPlan->iDist    = fftPlan->oDist;
          colPlan->oDist    = fftPlan->oDist;
          colPlan->hcfftlibtype  = fftPlan->hcfftlibtype;
          colPlan->originalLength  = fftPlan->originalLength;
          colPlan->acc  = fftPlan->acc;
          colPlan->exist  = fftPlan->exist;
          hcfftBakePlanInternal(fftPlan->planZ);
        }

        fftPlan->baked = true;
        return  HCFFT_SUCCEEDS;
      }
  }

  switch (fftPlan->gen) {
    case Stockham:
    case Transpose_GCN:
    case Copy: {
        //  For the radices that we have factored, we need to load/compile and build the appropriate HCC kernels
        fftPlan->GenerateKernel( plHandle, fftRepo, bakedPlanCount, fftPlan->exist);
        CompileKernels( plHandle, fftPlan->gen, fftPlan, fftPlan->plHandleOrigin, fftPlan->exist, fftPlan->originalLength, fftPlan->hcfftlibtype);
        bakedPlanCount++;
        fftPlan->baked    = true;
      }
      break;

    default:
      assert(false);
  }

  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanPrecision( const  hcfftPlanHandle plHandle,  hcfftPrecision* precision ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftGetPlanPrecision" ) );
  *precision = fftPlan->precision;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanPrecision(  hcfftPlanHandle plHandle,  hcfftPrecision precision ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftSetPlanPrecision" ) );
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;
  fftPlan->precision = precision;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanScale( const  hcfftPlanHandle plHandle,  hcfftDirection dir, float* scale ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftGetPlanScale" ) );

  if( dir == HCFFT_FORWARD) {
    *scale = (float)(fftPlan->forwardScale);
  } else {
    *scale = (float)(fftPlan->backwardScale);
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanScale(  hcfftPlanHandle plHandle,  hcfftDirection dir, float scale ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftSetPlanScale" ) );
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;

  if( dir == HCFFT_FORWARD) {
    fftPlan->forwardScale = scale;
  } else {
    fftPlan->backwardScale = scale;
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanBatchSize( const  hcfftPlanHandle plHandle, size_t* batchsize ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftGetPlanBatchSize" ) );
  *batchsize = fftPlan->batchSize;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanBatchSize( hcfftPlanHandle plHandle, size_t batchsize ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftSetPlanBatchSize" ) );
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;
  fftPlan->batchSize = batchsize;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanDim( const hcfftPlanHandle plHandle,  hcfftDim* dim, int* size ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetPlanDim" ) );
  *dim = fftPlan->dimension;

  switch( fftPlan->dimension ) {
    case HCFFT_1D: {
        *size = 1;
      }
      break;

    case HCFFT_2D: {
        *size = 2;
      }
      break;

    case HCFFT_3D: {
        *size = 3;
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanDim(  hcfftPlanHandle plHandle, const  hcfftDim dim ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetPlanDim" ) );

  // We resize the vectors in the plan to keep their sizes consistent with the value of the dimension
  switch( dim ) {
    case HCFFT_1D: {
        fftPlan->length.resize( 1 );
        fftPlan->inStride.resize( 1 );
        fftPlan->outStride.resize( 1 );
      }
      break;

    case HCFFT_2D: {
        fftPlan->length.resize( 2 );
        fftPlan->inStride.resize( 2 );
        fftPlan->outStride.resize( 2 );
      }
      break;

    case HCFFT_3D: {
        fftPlan->length.resize( 3 );
        fftPlan->inStride.resize( 3 );
        fftPlan->outStride.resize( 3 );
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;
  fftPlan->dimension = dim;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanLength( const  hcfftPlanHandle plHandle, const  hcfftDim dim, size_t* hcLengths ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetPlanLength" ) );

  if( hcLengths == NULL ) {
    return HCFFT_ERROR;
  }

  if( fftPlan->length.empty( ) ) {
    return HCFFT_ERROR;
  }

  switch( dim ) {
    case HCFFT_1D: {
        hcLengths[0] = fftPlan->length[0];
      }
      break;

    case HCFFT_2D: {
        if( fftPlan->length.size() < 2 ) {
          return HCFFT_ERROR;
        }

        hcLengths[0] = fftPlan->length[0];
        hcLengths[1 ] = fftPlan->length[1];
      }
      break;

    case HCFFT_3D: {
        if( fftPlan->length.size() < 3 ) {
          return HCFFT_ERROR;
        }

        hcLengths[0] = fftPlan->length[0];
        hcLengths[1 ] = fftPlan->length[1];
        hcLengths[2] = fftPlan->length[2];
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanLength(  hcfftPlanHandle plHandle, const  hcfftDim dim, const size_t* hcLengths ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetPlanLength" ) );

  if( hcLengths == NULL ) {
    return HCFFT_ERROR;
  }

  //  Simplest to clear any previous contents, because it's valid for user to shrink dimension
  fftPlan->length.clear( );

  switch( dim ) {
    case HCFFT_1D: {
        //  Minimum length size is 1
        if( hcLengths[0] == 0 ) {
          return HCFFT_ERROR;
        }

        fftPlan->length.push_back( hcLengths[0] );
      }
      break;

    case HCFFT_2D: {
        //  Minimum length size is 1
        if(hcLengths[0] == 0 || hcLengths[1] == 0 ) {
          return HCFFT_ERROR;
        }

        fftPlan->length.push_back( hcLengths[0] );
        fftPlan->length.push_back( hcLengths[1] );
      }
      break;

    case HCFFT_3D: {
        //  Minimum length size is 1
        if(hcLengths[0 ] == 0 || hcLengths[1] == 0 || hcLengths[2] == 0) {
          return HCFFT_ERROR;
        }

        fftPlan->length.push_back( hcLengths[0] );
        fftPlan->length.push_back( hcLengths[1] );
        fftPlan->length.push_back( hcLengths[2] );
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  fftPlan->dimension = dim;
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanInStride( const  hcfftPlanHandle plHandle, const  hcfftDim dim, size_t* hcStrides ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetPlanInStride" ) );

  if(hcStrides == NULL ) {
    return HCFFT_ERROR;
  }

  switch( dim ) {
    case HCFFT_1D: {
        if(fftPlan->inStride.size( ) > 0 ) {
          hcStrides[0] = fftPlan->inStride[0];
        } else {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_2D: {
        if( fftPlan->inStride.size( ) > 1 ) {
          hcStrides[0] = fftPlan->inStride[0];
          hcStrides[1] = fftPlan->inStride[1];
        } else {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_3D: {
        if( fftPlan->inStride.size( ) > 2 ) {
          hcStrides[0] = fftPlan->inStride[0];
          hcStrides[1] = fftPlan->inStride[1];
          hcStrides[2] = fftPlan->inStride[2];
        } else {
          return HCFFT_ERROR;
        }
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanInStride(  hcfftPlanHandle plHandle, const  hcfftDim dim, size_t* hcStrides ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetPlanInStride" ) );

  if( hcStrides == NULL ) {
    return HCFFT_ERROR;
  }

  //  Simplest to clear any previous contents, because it's valid for user to shrink dimension
  fftPlan->inStride.clear( );

  switch( dim ) {
    case HCFFT_1D: {
        fftPlan->inStride.push_back( hcStrides[0] );
      }
      break;

    case HCFFT_2D: {
        fftPlan->inStride.push_back( hcStrides[0] );
        fftPlan->inStride.push_back( hcStrides[1] );
      }
      break;

    case HCFFT_3D: {
        fftPlan->inStride.push_back( hcStrides[0] );
        fftPlan->inStride.push_back( hcStrides[1] );
        fftPlan->inStride.push_back( hcStrides[2] );
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanOutStride( const  hcfftPlanHandle plHandle, const  hcfftDim dim, size_t* hcStrides ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetPlanOutStride" ) );

  if( hcStrides == NULL ) {
    return HCFFT_ERROR;
  }

  switch( dim ) {
    case HCFFT_1D: {
        if( fftPlan->outStride.size() > 0 ) {
          hcStrides[0] = fftPlan->outStride[0];
        } else {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_2D: {
        if( fftPlan->outStride.size() > 1 ) {
          hcStrides[0] = fftPlan->outStride[0];
          hcStrides[1] = fftPlan->outStride[1];
        } else {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_3D: {
        if( fftPlan->outStride.size() > 2 ) {
          hcStrides[0] = fftPlan->outStride[0];
          hcStrides[1] = fftPlan->outStride[1];
          hcStrides[2] = fftPlan->outStride[2];
        } else {
          return HCFFT_ERROR;
        }
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanOutStride(  hcfftPlanHandle plHandle, const  hcfftDim dim, size_t* hcStrides ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetPlanOutStride" ) );

  if( hcStrides == NULL ) {
    return HCFFT_ERROR;
  }

  switch( dim ) {
    case HCFFT_1D: {
        fftPlan->outStride[0] = hcStrides[0];
      }
      break;

    case HCFFT_2D: {
        fftPlan->outStride[0] = hcStrides[0];
        fftPlan->outStride[1] = hcStrides[1];
      }
      break;

    case HCFFT_3D: {
        fftPlan->outStride[0] = hcStrides[0];
        fftPlan->outStride[1] = hcStrides[1];
        fftPlan->outStride[2] = hcStrides[2];
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked  = false;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanDistance( const  hcfftPlanHandle plHandle, size_t* iDist, size_t* oDist ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetPlanDistance" ) );
  *iDist = fftPlan->iDist;
  *oDist = fftPlan->oDist;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanDistance(  hcfftPlanHandle plHandle, size_t iDist, size_t oDist ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftSetPlanDistance" ) );
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked = false;
  fftPlan->iDist = iDist;
  fftPlan->oDist = oDist;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetLayout( const  hcfftPlanHandle plHandle,  hcfftIpLayout* iLayout,  hcfftOpLayout* oLayout ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan(plHandle, fftPlan, planLock );
  scopedLock sLock(*planLock, _T( " hcfftGetLayout" ) );
  *iLayout = fftPlan->ipLayout;
  *oLayout = fftPlan->opLayout;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetLayout(  hcfftPlanHandle plHandle,  hcfftIpLayout iLayout,  hcfftOpLayout oLayout ) {
  FFTRepo& fftRepo = FFTRepo::getInstance( );
  FFTPlan* fftPlan = NULL;
  lockRAII* planLock = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetLayout" ) );

  //  We currently only support a subset of formats
  switch( iLayout ) {
    case HCFFT_COMPLEX_INTERLEAVED: {
        if( (oLayout == HCFFT_HERMITIAN_INTERLEAVED) || (oLayout == HCFFT_HERMITIAN_PLANAR) || (oLayout == HCFFT_REAL)) {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_COMPLEX_PLANAR: {
        if( (oLayout == HCFFT_HERMITIAN_INTERLEAVED) || (oLayout == HCFFT_HERMITIAN_PLANAR) || (oLayout == HCFFT_REAL)) {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_HERMITIAN_INTERLEAVED: {
        if(oLayout != HCFFT_REAL) {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_HERMITIAN_PLANAR: {
        if(oLayout != HCFFT_REAL) {
          return HCFFT_ERROR;
        }
      }
      break;

    case HCFFT_REAL: {
        if((oLayout == HCFFT_REAL) || (oLayout == HCFFT_COMPLEX_INTERLEAVED) || (oLayout == HCFFT_COMPLEX_PLANAR)) {
          return HCFFT_ERROR;
        }
      }
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  //  We currently only support a subset of formats
  switch( oLayout ) {
    case HCFFT_COMPLEX_PLANAR:
    case HCFFT_COMPLEX_INTERLEAVED:
    case HCFFT_HERMITIAN_INTERLEAVED:
    case HCFFT_HERMITIAN_PLANAR:
    case HCFFT_REAL:
      break;

    default:
      return HCFFT_ERROR;
      break;
  }

  fftPlan->ipLayout = iLayout;
  fftPlan->opLayout = oLayout;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetResultLocation( const  hcfftPlanHandle plHandle,  hcfftResLocation* placeness ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetResultLocation" ) );
  *placeness  = fftPlan->location;
  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetResultLocation(  hcfftPlanHandle plHandle,  hcfftResLocation placeness ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetResultLocation" ) );
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked    = false;
  fftPlan->location = placeness;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftGetPlanTransposeResult( const  hcfftPlanHandle plHandle,  hcfftResTransposed* transposed ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftGetPlanTransposeResult" ) );
  *transposed = fftPlan->transposeType;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::hcfftSetPlanTransposeResult(  hcfftPlanHandle plHandle,  hcfftResTransposed transposed ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( plHandle, fftPlan, planLock );
  scopedLock sLock( *planLock, _T( " hcfftSetPlanTransposeResult" ) );
  //  If we modify the state of the plan, we assume that we can't trust any pre-calculated contents anymore
  fftPlan->baked    = false;
  fftPlan->transposeType  = transposed;
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::GetMax1DLength (size_t* longest ) const {
  switch(gen) {
    case Stockham:
      return GetMax1DLengthPvt<Stockham>(longest);

    case Copy: {
        *longest = 4096;
        return HCFFT_SUCCEEDS;
      }

    case Transpose_GCN: {
        *longest = 4096;
        return HCFFT_SUCCEEDS;
      }

    case Transpose_NONSQUARE: {
        *longest = 4096;
        return HCFFT_SUCCEEDS;
      }

    case Transpose_SQUARE: {
        *longest = 4096;
        return HCFFT_SUCCEEDS;
      }

    default:
      return HCFFT_ERROR;
  }
}

hcfftStatus  FFTPlan::GetKernelGenKey (FFTKernelGenKeyParams & params) const {
  switch(gen) {
    case Stockham:
      return GetKernelGenKeyPvt<Stockham>(params);

    case Copy:
      return GetKernelGenKeyPvt<Copy>(params);

    case Transpose_GCN:
      return GetKernelGenKeyPvt<Transpose_GCN>(params);

    case Transpose_NONSQUARE:
      return GetKernelGenKeyPvt<Transpose_NONSQUARE>(params);

    case Transpose_SQUARE:
      return GetKernelGenKeyPvt<Transpose_SQUARE>(params);

    default:
      return HCFFT_ERROR;
  }
}

hcfftStatus  FFTPlan::GetWorkSizes (std::vector<size_t> & globalws, std::vector<size_t> & localws) const {
  switch(gen) {
    case Stockham:
      return GetWorkSizesPvt<Stockham>(globalws, localws);

    case Copy:
      return GetWorkSizesPvt<Copy>(globalws, localws);

    case Transpose_GCN:
      return GetWorkSizesPvt<Transpose_GCN>(globalws, localws);

    case Transpose_NONSQUARE:
      return GetWorkSizesPvt<Transpose_NONSQUARE>(globalws, localws);

    case Transpose_SQUARE:
      return GetWorkSizesPvt<Transpose_SQUARE>(globalws, localws);

    default:
      return HCFFT_ERROR;
  }
}

hcfftStatus  FFTPlan::GenerateKernel (const hcfftPlanHandle plHandle, FFTRepo & fftRepo, size_t count, bool exist) const {
  switch(gen) {
    case Stockham:
      return GenerateKernelPvt<Stockham>(plHandle, fftRepo, count, exist);

    case Copy:
      return GenerateKernelPvt<Copy>(plHandle, fftRepo, count, exist);

    case Transpose_GCN:
      return GenerateKernelPvt<Transpose_GCN>(plHandle, fftRepo, count, exist);

    case Transpose_NONSQUARE:
      return GenerateKernelPvt<Transpose_NONSQUARE>(plHandle, fftRepo, count, exist);

    case Transpose_SQUARE:
      return GenerateKernelPvt<Transpose_SQUARE>(plHandle, fftRepo, count, exist);

    default:
      return HCFFT_ERROR;
  }
}

hcfftStatus FFTPlan::hcfftDestroyPlan( hcfftPlanHandle* plHandle ) {
  FFTRepo& fftRepo  = FFTRepo::getInstance( );
  FFTPlan* fftPlan  = NULL;
  lockRAII* planLock  = NULL;
  fftRepo.getPlan( *plHandle, fftPlan, planLock );

  if( fftPlan->kernelPtr) {
    fftPlan->kernelPtr = NULL;
  }

  //  Recursively destroy subplans, that are used for higher dimensional FFT's
  if( fftPlan->planX ) {
    hcfftDestroyPlan( &fftPlan->planX );
  }

  if( fftPlan->planY ) {
    hcfftDestroyPlan( &fftPlan->planY );
  }

  if( fftPlan->planZ ) {
    hcfftDestroyPlan( &fftPlan->planZ );
  }

  if( fftPlan->planTX ) {
    hcfftDestroyPlan( &fftPlan->planTX );
  }

  if( fftPlan->planTY ) {
    hcfftDestroyPlan( &fftPlan->planTY );
  }

  if( fftPlan->planTZ ) {
    hcfftDestroyPlan( &fftPlan->planTZ );
  }

  if( fftPlan->planRCcopy ) {
    hcfftDestroyPlan( &fftPlan->planRCcopy );
  }

  if( fftPlan->planCopy ) {
    hcfftDestroyPlan( &fftPlan->planCopy );
  }

  fftPlan->ReleaseBuffers();

  if(kernelHandle) {
    if(dlclose(kernelHandle)) {
      char* err = dlerror();
      std::cout << " Failed to close KernHandle " << err;
      free(err);
      exit(1);
    }

    kernelHandle = NULL;
  }

  fftRepo.deletePlan( plHandle );
  return HCFFT_SUCCEEDS;
}

hcfftStatus FFTPlan::ReleaseBuffers () {
  hcfftStatus result = HCFFT_SUCCEEDS;

  if( NULL != intBuffer ) {
    if( hc::am_free(intBuffer) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    intBuffer = NULL;
  }

  if( NULL != intBufferRC ) {
    if( hc::am_free(intBufferRC) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    intBufferRC = NULL;
  }

  if( NULL != intBufferC2R ) {
    if( hc::am_free(intBufferC2R) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    intBufferC2R = NULL;
  }

  if( NULL != twiddles ) {
    if( hc::am_free(twiddles) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    twiddles = NULL;
  }

  if( NULL != twiddleslarge ) {
    if( hc::am_free(twiddleslarge) != AM_SUCCESS) {
      return HCFFT_INVALID;
    }

    twiddleslarge = NULL;
  }

  return result;
}

size_t FFTPlan::ElementSize() const {
  return ((precision == HCFFT_DOUBLE) ? sizeof(std::complex<double> ) : sizeof(std::complex<float>));
}

/*----------------------------------------------------FFTPlan-----------------------------------------------------------------------------*/

/*---------------------------------------------------FFTRepo--------------------------------------------------------------------------------*/
hcfftStatus FFTRepo::createPlan( hcfftPlanHandle* plHandle, FFTPlan*& fftPlan ) {
  scopedLock sLock( lockRepo, _T( "insertPlan" ) );
  //  We keep track of this memory in our own collection class, to make sure it's freed in releaseResources
  //  The lifetime of a plan is tracked by the client and is freed when the client calls ::hcfftDestroyPlan()
  fftPlan = new FFTPlan;
  //  We allocate a new lock here, and expect it to be freed in ::hcfftDestroyPlan();
  //  The lifetime of the lock is the same as the lifetime of the plan
  lockRAII* lockPlan  = new lockRAII;
  //  Add and remember the fftPlan in our map
  repoPlans[ planCount ] = std::make_pair( fftPlan, lockPlan );
  //  Assign the user handle the plan count (unique identifier), and bump the count for the next plan
  *plHandle = planCount++;
  return  HCFFT_SUCCEEDS;
}


hcfftStatus FFTRepo::getPlan( hcfftPlanHandle plHandle, FFTPlan*& fftPlan, lockRAII*& planLock ) {
  scopedLock sLock( lockRepo, _T( "getPlan" ) );
  //  First, check if we have already created a plan with this exact same FFTPlan
  repoPlansType::iterator iter  = repoPlans.find( plHandle );

  if( iter == repoPlans.end( ) ) {
    return  HCFFT_ERROR;
  }

  //  If plan is valid, return fill out the output pointers
  fftPlan   = iter->second.first;
  planLock  = iter->second.second;
  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTRepo::deletePlan( hcfftPlanHandle* plHandle ) {
  scopedLock sLock( lockRepo, _T( "deletePlan" ) );
  //  First, check if we have already created a plan with this exact same FFTPlan
  repoPlansType::iterator iter  = repoPlans.find( *plHandle );

  if( iter == repoPlans.end( ) ) {
    return  HCFFT_ERROR;
  }

  //  We lock the plan object while we are in the process of deleting it
  {
    scopedLock sLock( *iter->second.second, _T( "hcfftDestroyPlan" ) );
    //  Delete the FFTPlan
    delete iter->second.first;
  }
  //  Delete the lockRAII
  delete iter->second.second;
  //  Remove entry from our map object
  repoPlans.erase( iter );
  //  Clear the client's handle to signify that the plan is gone
  *plHandle = 0;
  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTRepo::setProgramEntryPoints( const hcfftGenerators gen, const hcfftPlanHandle& handle,
    const FFTKernelGenKeyParams& fftParam, const char* kernel_fwd,
    const char* kernel_back) {
  scopedLock sLock( lockRepo, _T( "setProgramEntryPoints" ) );
  fftRepoKey key = std::make_pair( gen, handle );
  fftRepoValue& fft = mapFFTs[ key ];
  fft.EntryPoint_fwd  = kernel_fwd;
  fft.EntryPoint_back = kernel_back;
  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTRepo::getProgramEntryPoint( const hcfftGenerators gen, const hcfftPlanHandle& handle,
    const FFTKernelGenKeyParams& fftParam, hcfftDirection dir,
    std::string& kernel) {
  scopedLock sLock( lockRepo, _T( "getProgramEntryPoint" ) );
  fftRepoKey key = std::make_pair( gen, handle );
  fftRepo_iterator pos = mapFFTs.find( key );

  if( pos == mapFFTs.end( ) ) {
    return  HCFFT_ERROR;
  }

  switch (dir) {
    case HCFFT_FORWARD:
      kernel = pos->second.EntryPoint_fwd;
      break;

    case HCFFT_BACKWARD:
      kernel = pos->second.EntryPoint_back;
      break;

    default:
      assert (false);
      return HCFFT_ERROR;
  }

  if (0 == kernel.size()) {
    return  HCFFT_ERROR;
  }

  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTRepo::setProgramCode( const hcfftGenerators gen, const hcfftPlanHandle& handle, const FFTKernelGenKeyParams& fftParam, const std::string& kernel) {
  scopedLock sLock( lockRepo, _T( "setProgramCode" ) );
  fftRepoKey key = std::make_pair( gen, handle );
  // Prefix copyright statement at the top of generated kernels
  std::stringstream ss;
  ss <<
     "/* ************************************************************************\n"
     " * Copyright 2013 MCW, Inc.\n"
     " *\n"
     " * ************************************************************************/"
     << std::endl << std::endl;
  std::string prefixCopyright = ss.str();
  mapFFTs[ key ].ProgramString = prefixCopyright + kernel;
  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTRepo::getProgramCode( const hcfftGenerators gen, const hcfftPlanHandle& handle, const FFTKernelGenKeyParams& fftParam, std::string& kernel) {
  scopedLock sLock( lockRepo, _T( "getProgramCode" ) );
  fftRepoKey key = std::make_pair( gen, handle );
  fftRepo_iterator pos = mapFFTs.find( key);

  if( pos == mapFFTs.end( ) ) {
    return  HCFFT_ERROR;
  }

  kernel = pos->second.ProgramString;
  return  HCFFT_SUCCEEDS;
}

hcfftStatus FFTRepo::releaseResources( ) {
  scopedLock sLock( lockRepo, _T( "releaseResources" ) );

  //  Free all memory allocated in the repoPlans; represents cached plans that were not destroyed by the client
  //
  for( repoPlansType::iterator iter = repoPlans.begin( ); iter != repoPlans.end( ); ++iter ) {
    FFTPlan* plan = iter->second.first;
    lockRAII* lock  = iter->second.second;

    if( plan != NULL ) {
      delete plan;
    }

    if( lock != NULL ) {
      delete lock;
    }
  }

  //  Reset the plan count to zero because we are guaranteed to have destroyed all plans
  planCount = 1;
  //  Release all strings
  mapFFTs.clear( );
  return  HCFFT_SUCCEEDS;
}
/*------------------------------------------------FFTRepo----------------------------------------------------------------------------------*/
