
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
std::atomic<float> g_backbuffer_aspect{1920.f/1080.f};
double now_s(){ return GetTickCount64()/1000.0; }
void vlog(const char* f,...){ va_list a; va_start(a,f); vprintf(f,a); va_end(a); printf("\n"); }
size_t cam_probe_test_scan(const uint8_t*, size_t, float, uintptr_t*, size_t);
int main(){
  static uint8_t buf[1<<20]; memset(buf,0,sizeof(buf));
  // decoys: aspect alone, bad quat
  *(float*)(buf+0x100+0x30)=1920.f/1080.f;
  size_t offs[2]={0x1238+0x8-0x8 /*aligned 8*/, 0x80000};
  for(size_t o: offs){ uint8_t* b=buf+o; int64_t* p=(int64_t*)b; p[0]=123456789; p[1]=-5; p[2]=42;
    float* f=(float*)b; f[6]=0; f[7]=0.3826834f; f[8]=0; f[9]=0.9238795f; f[10]=4.0f; f[11]=0.8726646f; f[12]=1920.f/1080.f; f[13]=0.1f; f[14]=20000.f; }
  // decoy with non-unit quat
  { uint8_t* b=buf+0x9000; float* f=(float*)b; f[6]=1; f[7]=1; f[8]=0; f[9]=0; f[10]=4; f[11]=0.87f; f[12]=1920.f/1080.f; f[13]=0.1f; f[14]=20000.f; }
  uintptr_t out[32]; size_t n=cam_probe_test_scan(buf,sizeof(buf),1920.f/1080.f,out,32);
  printf("found %zu:", n); for(size_t i=0;i<n;i++) printf(" +0x%zx", (size_t)(out[i]-(uintptr_t)buf)); printf("\n");
  return (n==2 && out[0]-(uintptr_t)buf==0x1238 && out[1]-(uintptr_t)buf==0x80000)?0:1;
}
