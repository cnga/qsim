/*****************************************************************************\
* Qemu Simulation Framework (qsim)                                            *
* Qsim is a modified version of the Qemu emulator (www.qemu.org), couled     *
* a C++ API, for the use of computer architecture researchers.                *
*                                                                             *
* This work is licensed under the terms of the GNU GPL, version 2. See the    *
* COPYING file in the top-level directory.                                    *
\*****************************************************************************/
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>

#include <qsim.h>

using Qsim::OSDomain;

using std::ostream;

static const size_t cacheLineSizeLog2 = 6;
static const size_t cacheLineSize     = 1<<cacheLineSizeLog2;

#define KB(x) ((x) << 10)
#define MB(x) ((x) << 20)

class CacheHitCounter {

    static const size_t  depthLog2 = 4;
    static const size_t  depth     = 1<<depthLog2;
    size_t  widthLog2;
    size_t  width;
    size_t  widthMask;
    size_t  hits;
    size_t  misses;
    size_t  addressesLen;
    size_t* addresses;
    size_t  maxSize;

    CacheHitCounter & operator =(CacheHitCounter const & CacheHitProfile1);
    CacheHitCounter(CacheHitCounter const &);

    public:
    CacheHitCounter() {}
    CacheHitCounter(size_t maxSizeLog2) {
        maxSize         = size_t(1)<<maxSizeLog2;
        widthLog2       = maxSizeLog2 - cacheLineSizeLog2 - depthLog2;
        width           = size_t(1)<<widthLog2;
        widthMask       = width-1;
        addresses		= new size_t[addressesLen];

        clear();
    }

    void initialize(size_t size) {
        maxSize         = size;
        width           = size / ((1<<depthLog2) * cacheLineSize);
        widthMask       = width-1;
        addressesLen    = depth*width;

        addresses		= new size_t[addressesLen];

        clear();
    }

    void clear() {
        hits   = 0;
        misses = 0;
        for (size_t i = 0; i < addressesLen; i++) addresses[i] = 0;
    }

    void clearAddresses() 
    {
        memset(addresses, 0, addressesLen * sizeof(size_t));
        //for (size_t i = 0; i < addressesLen; i++) addresses[i] = 0;
    }

    ~CacheHitCounter() {
        delete [] addresses;
    }

    void insert(size_t cacheLine, size_t hashedCacheLine) {

        size_t col = hashedCacheLine % width; 
        size_t* c  = &addresses[col*depth];
        size_t  pc = cacheLine;
        size_t  r  = 0;
        for (; r < depth; r++) {
            size_t oldC = c[r];
            c[r] = pc;
            if (oldC == cacheLine) {
                hits++;
                return;
            }
            pc = oldC;
        }
        misses++;
    };

    size_t getHits() {
        return hits;
    }

    double getHitRatio() {
        size_t total = hits + misses;

        return (double)hits / total;
    }

    double getMissRatio() {
        size_t total = hits + misses;

        return (double)misses / total;
    }

    size_t getTotalAccesses() { return hits + misses; }

    size_t getCacheSize()     { return maxSize / MB(1); }

    void PrintConfig() {
        printf("CacheSize %lu, width %lu, addressesLen %lu\n", maxSize / MB(1), width, addressesLen);
    }
};

class TraceWriter {
    public:
        TraceWriter(OSDomain &osd, ostream &tracefile) : 
            osd(osd), tracefile(tracefile), finished(false) 
        { 
            //osd.set_app_start_cb(this, &TraceWriter::app_start_cb); 
            counter.initialize(MB(8));
        }

        bool hasFinished() { return finished; }

        int app_start_cb(int c) {
            static bool ran = false;
            if (!ran) {
                ran = true;
                osd.set_mem_cb(this, &TraceWriter::mem_cb);

                return 1;
            }
        }

        int mem_cb(int c, uint64_t v, uint64_t p, uint8_t s, int w) {
            uint64_t hashed_addr = v ^ (v >> 13);
            counter.insert(v, hashed_addr);
            return 0;
        }

        double get_hit_ratio() { return counter.getHitRatio(); }

    private:
        OSDomain &osd;
        ostream &tracefile;
        bool finished;

        static const char * itype_str[];
        CacheHitCounter counter;
};

const char *TraceWriter::itype_str[] = {
    "QSIM_INST_NULL",
    "QSIM_INST_INTBASIC",
    "QSIM_INST_INTMUL",
    "QSIM_INST_INTDIV",
    "QSIM_INST_STACK",
    "QSIM_INST_BR",
    "QSIM_INST_CALL",
    "QSIM_INST_RET",
    "QSIM_INST_TRAP",
    "QSIM_INST_FPBASIC",
    "QSIM_INST_FPMUL",
    "QSIM_INST_FPDIV"
};

int main(int argc, char** argv) {
    using std::istringstream;
    using std::ofstream;

    std::string qsim_prefix(getenv("QSIM_PREFIX"));
    ofstream *outfile(NULL);

    unsigned n_cpus = 1;

    // Read number of CPUs as a parameter. 
    if (argc >= 2) {
        istringstream s(argv[1]);
        s >> n_cpus;
    }

    // Read trace file as a parameter.
    if (argc >= 3) {
        outfile = new ofstream(argv[2]);
    } else 
        outfile = new ofstream("trace.log");

    OSDomain *osd_p(NULL);

    if (argc >= 4) {
        // Create new OSDomain from saved state.
        osd_p = new OSDomain(argv[3]);
        n_cpus = osd_p->get_n();
    } else {
        osd_p = new OSDomain(n_cpus, qsim_prefix + "/../arm_images/vmlinuz-3.2.0-4-vexpress");
    }
    OSDomain &osd(*osd_p);

    // Attach a TraceWriter if a trace file is given.
    TraceWriter tw(osd, outfile?*outfile:std::cout);

    // If this OSDomain was created from a saved state, the app start callback was
    // received prior to the state being saved.
    //if (argc >= 4) tw.app_start_cb(0);

    osd.connect_console(std::cout);

	unsigned long inst_per_iter = 1000000;
    tw.app_start_cb(0);
    // The main loop: run until 'finished' is true.
    unsigned k = 0; // outer loop counter
    while (!tw.hasFinished()) {
        for (unsigned i = 0; i < 100; i++) {
            for (unsigned long j = 0; j < n_cpus; j++) {
                osd.run(j, 1000000);
            }
            std::cerr << ((i+1) + k * 100) * inst_per_iter / 1e6 << " million instructions, hit ratio " <<
                tw.get_hit_ratio() << std::endl;
            fflush(NULL);
        }
        k++;
        osd.timer_interrupt();
    }

    if (outfile) { outfile->close(); }
    delete outfile;

    delete osd_p;

    return 0;
}