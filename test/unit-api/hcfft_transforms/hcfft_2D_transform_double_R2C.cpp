#include "hcfft.h"
#include "../gtest/gtest.h"
#include "fftw3.h"
#include "helper_functions.h"
#include "hc_am.hpp"
#include "hcfftlib.h"

TEST(hcfft_2D_transform_test, func_correct_2D_transform_D2Z ) {
  putenv((char*)"GTEST_BREAK_ON_FAILURE=0");
  size_t N1, N2;
  N1 = my_argc > 1 ? atoi(my_argv[1]) : 8;
  N2 = my_argc > 2 ? atoi(my_argv[2]) : 8;
  hcfftHandle plan;
  hcfftResult status  = hcfftPlan2d(&plan, N1, N2,  HCFFT_D2Z);
  EXPECT_EQ(status, HCFFT_SUCCESS);
  int Rsize = N2 * N1;
  int Csize = N2 * (1 + N1 / 2);
  hcfftDoubleReal* input = (hcfftDoubleReal*)calloc(Rsize, sizeof(hcfftDoubleReal));
  int seed = 123456789;
  srand(seed);

  // Populate the input
  for(int i = 0; i < Rsize ; i++) {
    input[i] = i%8;
  }

  hcfftDoubleComplex* output = (hcfftDoubleComplex*)calloc(Csize, sizeof(hcfftDoubleComplex));
  std::vector<hc::accelerator> accs = hc::accelerator::get_all();
  assert(accs.size() && "Number of Accelerators == 0!");
  hc::accelerator_view accl_view = accs[1].get_default_view();
  hcfftDoubleReal* idata = hc::am_alloc(Rsize * sizeof(hcfftDoubleReal), accs[1], 0);
  accl_view.copy(input, idata, sizeof(hcfftDoubleReal) * Rsize);
  hcfftDoubleComplex* odata = hc::am_alloc(Csize * sizeof(hcfftDoubleComplex), accs[1], 0);
  accl_view.copy(output, odata, sizeof(hcfftDoubleComplex) * Csize);
  status = hcfftExecD2Z(plan, idata, odata);
  EXPECT_EQ(status, HCFFT_SUCCESS);
  accl_view.copy(odata, output, sizeof(hcfftDoubleComplex) * Csize);
  status =  hcfftDestroy(plan);
  EXPECT_EQ(status, HCFFT_SUCCESS);
  //FFTW work flow
  // input output arrays
  double *in; fftw_complex* out;
  fftw_plan p;
  in = (double*) fftw_malloc(sizeof(double) * Rsize);
  // Populate inputs
  for(int i = 0; i < Rsize ; i++) {
    in[i] = input[i];
  }
  out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * Csize);
  // 2D forward plan
  p = fftw_plan_dft_r2c_2d(N2, N1, in, out, FFTW_ESTIMATE | FFTW_R2HC);;
  // Execute R2C
  fftw_execute(p);

  // Check RMSE: If fails move on to pointwise comparison 
  if (JudgeRMSEAccuracyComplex<fftw_complex, hcfftDoubleComplex>(out, output, Csize)) {
    //Check Real Outputs
    for (int i =0; i < Csize; i++) {
      EXPECT_NEAR(out[i][0] , output[i].x, 0.1); 
    }
    //Check Imaginary Outputs
    for (int i =0; i < Csize; i++) {
      EXPECT_NEAR(out[i][1] , output[i].y, 0.1); 
    }
  }

  //Free up resources
  fftw_destroy_plan(p);
  free(input);
  free(output);
  hc::am_free(idata);
  hc::am_free(odata);
}


