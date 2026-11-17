#include "../metal/pyrowave_wavelet.hpp"
#include <cassert>
#include <cstdio>
int main(int argc,char**argv){assert(argc==2);assert(PyroWave::cdf53_enabled()==(argv[1][0]=='1'));puts("Explicit paired wavelet selection passed");}
