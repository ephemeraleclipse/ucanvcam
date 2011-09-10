
#include "Vcam.h"

#include <yarp/os/all.h>
#include <yarp/sig/all.h>
#include <yarp/dev/all.h>

using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::dev;

YARP_DECLARE_PLUGINS(vcammod);

#include "Effects.h"

#include "modules/ISourceLister.h"

#ifdef MERGE_SERVICE
#include "activex/Register.h"
#endif

#ifdef SHMEM_SERVICE
#include "ShmemBus.h"
#include "ShmemImage.h"
#endif

static bool touched = false;

#define DBG

extern int g_hinstance;
extern int g_hwnd;

#ifdef MERGE_SERVICE
Semaphore service_sema(1);
bool getServerImage(ImageOf<PixelRgb>& img) {
  touched = true;
  return false;
}
#endif


class VcamWin : public Vcam {
private:
  Network yarp;
  //DriverCollection dev;
  PolyDriver source;
  IFrameGrabberImage *grabber;
  ISourceLister *lister;
  ImageOf<PixelRgb> cache, proc;
  Bottle sources;
  bool output;
  bool stopOutputReq;
#ifdef SHMEM_SERVICE
  ShmemBus bus;
  ShmemImage busImage;
  ShmemImageHeader header;
#endif
  Semaphore service, inputMutex, stopped;
  ConstString outputName;
  ConstString sourceName;
public:
  VcamWin() : service(1), inputMutex(1), stopped(0), busImage(bus) {
    output = false;
    cache.setQuantum(1);
    getList();
    sourceName = "none";
    outputName = "none";
    open("test");
    memset((void*)(&header),0,sizeof(header));
    stopOutputReq = false;
    YARP_REGISTER_PLUGINS(vcammod);
  }

  virtual ~VcamWin() {
    service.wait();
    printf("Shut down output\n");
    stopOutput();
    printf("Shut down source\n");
    source.close();
    printf("Shut down vcam\n");
    grabber = NULL;
    service.post();
  }

  bool getList() {
    printf("Getting list\n");  fflush(stdout);
    sources.clear();
    sources.addString("test");
    Property pSource;
    pSource.put("device","vdub");
    pSource.put("passive","1");
    printf("Trying to open\n");  fflush(stdout);
    bool ok = source.open(pSource);
    lister = NULL;
    source.view(lister);
    if (lister!=NULL) {
      printf("Checking sources... (%ld)\n", (long int) lister);  fflush(stdout);
      //lister->getSources();
      sources.append(lister->getSources());
      printf("Done Checking sources... (%ld)\n", (long int) lister);  fflush(stdout);
    }
    source.close();
    return true;
    
  }

  bool open(const char *name) {
    inputMutex.wait();
    source.close();
    grabber = NULL;
    Property pSource;
    pSource.put("device","vdub");
    
    pSource.put("source",name);
    //pSource.put("v4l",1);
    //pSource.put("v4ldevice","/dev/video0");
    //pSource.put("v4ldevice","/dev/video2");
    pSource.put("width",320);
    pSource.put("height",240);
    pSource.put("w",320);
    pSource.put("h",240);
    pSource.put("mode","ball");
    pSource.put("hinstance",g_hinstance);
    pSource.put("hwnd",g_hwnd);
    //pSource.put("source","/scratch/camera/dcim/135canon/mvi_3549.avi");
    bool ok = false;
    if (ConstString("test")!=name) {
      ok = source.open(pSource);
    }
    if (!ok) {
        pSource.put("device","test_grabber");
        pSource.put("width",320);
        pSource.put("height",240);
        pSource.put("mode","grid");
        source.open(pSource);
    }
	grabber = NULL;
	lister = NULL;
    source.view(grabber);    
    source.view(lister);
    sourceName = name;
    inputMutex.post();
    return true;
  }

  virtual bool isImage() {
    if (grabber==NULL) {
      return false;
    }
    return true; // not quite true...
  }

  virtual yarp::sig::Image *getImage() {
    printf("no longer implemented\n");
    exit(1);
    bool result = grabber->getImage(cache);
    if (!result) return NULL;
    Effects::apply(cache,proc);
    if (touched) {
      proc.zero();
    }
    return &proc;
  }

  virtual bool getImage(ImageOf<PixelRgb>& img) {
    inputMutex.wait();
    bool result = false;
    if (grabber!=NULL) {
      result = grabber->getImage(cache);
    }
    inputMutex.post();
    if (!result) return false;

#ifdef SHMEM_SERVICE
    bool shouldEnd = false;
    service.wait();
    if (output) {
      bus.beginWrite();
      /*
	if (bus.buffer()!=img.getRawImage()) {
	img.setQuantum(1);
	img.setExternal(bus.buffer(),cache.width(),cache.height());
	printf("*** redirected cache\n");
	}
      */
      shouldEnd = true;
      Effects::apply(cache,busImage.getImage());
      if (touched) {
	busImage.getImage().zero();
      }
      header.tick++;
	  header.w = img.width();
	  header.h = img.height();
      busImage.getHeader() = header;
      if (busImage.getImage().getRawImage()!=img.getRawImage()) {
	img.setQuantum(1);
	img.setExternal(busImage.getImage().getRawImage(),
			busImage.getImage().width(),
			busImage.getImage().height());
	printf("*** redirected cache\n");
      }
    } else {
      Effects::apply(cache,img);
      if (touched) {
	img.zero();
      }
    }
    if (shouldEnd) {
      bus.endWrite();      
    }
    service.post();
#else
    Effects::apply(cache,img);
    if (touched) {
      img.zero();
    }
#endif
    return true;
  }

  virtual yarp::os::Bottle getSources() {
    return sources;
  }

  virtual void setSource(const char *name) {
    if (sources.size()<1) {
      getSources();
    }
    printf("Should switch source to %s\n", name);
    open(name);
  }


  virtual yarp::os::Bottle getOutputs() {
    yarp::os::Bottle b;
    b.addString("none");
#ifdef MERGE_SERVICE
    b.addString("ucanvcam virtual camera");
#endif
#ifdef SHMEM_SERVICE
    b.addString("ucanvcam shared camera");
#endif
    return b;
  }

  virtual bool setOutput(const char *name) {
    printf("Should set output to %s\n", name);
    if (ConstString(name)=="none") {
      stopOutput();
    } else {
      stopOutput();
      startOutput(name);
    }
    outputName = name;
    return true;
  }


  void stopOutput() {
    if (output) {
#ifdef MERGE_SERVICE
      UnregisterService();
#endif
#ifdef SHMEM_SERVICE
      //service.wait();
      output = false;
      bus.fini();
      //service.post();
#endif // SHMEM_SERVICE
    }
  }

  void startOutput(const char *name) {
    stopOutput();
    if (strlen(name)>1) {
      printf("*** starting output %s\n", name);
#ifdef MERGE_SERVICE
      RegisterService();
#endif
#ifdef SHMEM_SERVICE
      service.wait();
      bus.init();
      service.post();
#endif // SHMEM_SERVICE
      output = true;
    }
  }


};



Vcam *getVcam() {
  return new VcamWin;
}


