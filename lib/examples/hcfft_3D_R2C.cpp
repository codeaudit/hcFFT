#include "hcfft.h"

int main(int argc, char *argv[])
{
  int N1 = atoi(argv[1]);
  int N2 = atoi(argv[2]);
  int N3 = atoi(argv[3]);

  hcfftHandle *plan = NULL;
  hcfftResult status  = hcfftPlan3d(plan, N1, N2, N3, HCFFT_R2C);
  assert(status == HCFFT_SUCCESS);
  int Rsize = N3 * N2 * N1;
  int Csize = N3 * N2 * (1 + N1 / 2);
  hcfftReal *input = (hcfftReal*)calloc(Rsize, sizeof(hcfftReal));
  int seed = 123456789;
  srand(seed);
  // Populate the input
  for(int i = 0; i < Rsize ; i++) {
    input[i] = rand();
  }

  hcfftComplex *output = (hcfftComplex*)calloc(Csize, sizeof(hcfftComplex));

  std::vector<accelerator> accs = accelerator::get_all();
  assert(accs.size() && "Number of Accelerators == 0!");

  hcfftReal *idata = hc::am_alloc(Rsize * sizeof(hcfftReal), accs[1], 0);
  hc::am_copy(idata, input, sizeof(hcfftReal) * Rsize);

  hcfftComplex *odata = hc::am_alloc(Csize * sizeof(hcfftComplex), accs[1], 0);
  hc::am_copy(odata,  output, sizeof(hcfftComplex) * Csize);

  status = hcfftExecR2C(*plan, idata, odata);
  assert(status == HCFFT_SUCCESS);

  hc::am_copy(output, odata, sizeof(hcfftComplex) * Csize);
  status =  hcfftDestroy(*plan);
  assert(status == HCFFT_SUCCESS);

  free(input);
  free(output);

  hc::am_free(idata);
  hc::am_free(odata);
}
