#include <stdint.h>
#include <unistd.h>
#include <xen/xen.h>
#include <xen/grant_table.h>  // Kernel interface
#include <xen/sys/gntalloc.h> // Userspace interface
#include <atomic>

#include "osv_xen.hh"
#include "gntalloc.hh"

namespace xen {

gntref invalid_ref;

// FIXME: Most of the destructors are yet to be coded
//

class userspace_grant_head : public grant_head {
    std::atomic<int> _ref_head = { 0 };
    std::vector<gntref> _refs;
public:
    userspace_grant_head(std::vector<gntref> v) : _refs(v) {}
    virtual gntref new_ref() override;
    virtual gntref new_ref(void *addr, size_t size) override;
    virtual void free_ref(gntref& ref);
};

class userspace_gntalloc : public gntalloc {
    file_desc _gntalloc;
    struct ioctl_gntalloc_alloc_gref *get_gref(unsigned nr_ents);

public:
    explicit userspace_gntalloc(unsigned otherend);
    ~userspace_gntalloc();
    virtual gntref alloc_ref() override;
    virtual grant_head *alloc_ref(unsigned refs) override;
};

userspace_gntalloc::userspace_gntalloc(unsigned otherend)
    : gntalloc(otherend)
    , _gntalloc(file_desc::open("/dev/xen/gntalloc", O_RDWR)) {

}


struct ioctl_gntalloc_alloc_gref*
userspace_gntalloc::get_gref(unsigned nr_ents)
{
    struct ioctl_gntalloc_alloc_gref *gref;
    gref = static_cast<struct ioctl_gntalloc_alloc_gref *>(malloc(sizeof(*gref) + nr_ents * sizeof(gref->gref_ids[0])));
    gref->domid = _otherend;
    gref->flags = GNTALLOC_FLAG_WRITABLE;
    gref->count = nr_ents;
    _gntalloc.ioctl<struct ioctl_gntalloc_alloc_gref>(IOCTL_GNTALLOC_ALLOC_GREF, *gref);

    return gref;
}

grant_head *userspace_gntalloc::alloc_ref(unsigned nr_ents) {

    auto gref = get_gref(nr_ents);

    char *addr = (char *)_gntalloc.map_shared_rw(4096 * nr_ents, gref->index);

    std::vector<gntref> v;
    for (unsigned i = 0; i < nr_ents; ++i) {
        auto ref = gref->gref_ids[i];
        auto *page = addr + 4096 * i;
        v.push_back({int(ref), page});
    }

    free(gref);
    return new userspace_grant_head(v);
}

gntref userspace_gntalloc::alloc_ref() {

    auto gref = get_gref(1);

    void *addr = _gntalloc.map_shared_rw(4096, gref->index);
    auto p = gntref{int(gref->gref_ids[0]), addr};
    free(gref);
    return p;
}

gntref userspace_grant_head::new_ref() {
    return _refs[_id++ % _refs.size()];
}

gntref userspace_grant_head::new_ref(void *addr, size_t size) {
    gntref& ref = _refs[_id % _refs.size()];
    memcpy(ref.page, addr, size);
    return ref;
}

void userspace_grant_head::free_ref(gntref& ref) {
    abort();
}

#ifdef HAVE_OSV
class kernel_gntalloc;

class kernel_grant_head : public grant_head {
    uint32_t _head;
public:
    kernel_grant_head(uint32_t head) : _head(head) {}
    virtual gntref new_ref() override;
    virtual gntref new_ref(void *addr, size_t size) override;
    virtual void free_ref(gntref& ref);
};

class kernel_gntalloc : public gntalloc {
    static constexpr int _tx_grants = 256;
    static constexpr int _rx_grants = 256;

    uint32_t tx_head, rx_head;
protected:
    void *new_frame();
public:
    kernel_gntalloc(unsigned otherend) : gntalloc(otherend) {}

    virtual gntref alloc_ref() override;
    virtual grant_head *alloc_ref(unsigned refs) override;
    friend class kernel_grant_head;
};

// FIXME: It is not actually that far-fetched to run seastar ontop of raw pv,
// and in that case we'll need to have an extra step here
inline uint64_t
virt_to_mfn(void *virt) {
    return virt_to_phys(virt) >> 12;
}

gntref kernel_grant_head::new_ref() {
    auto gnt = dynamic_cast<kernel_gntalloc *>(gntalloc::instance());

    auto ref = gnttab_claim_grant_reference(&_head);
    auto page = gnt->new_frame();
    gnttab_grant_foreign_access_ref(ref, gnt->_otherend, virt_to_mfn(page), 0);
    return gntref(ref, page);
}

gntref kernel_grant_head::new_ref(void *addr, size_t size) {

    auto gnt = dynamic_cast<kernel_gntalloc *>(gntalloc::instance());

    // FIXME: if we can guarantee that the packet allocation came from malloc, not
    // mmap, we can grant it directly, without copying. We would also have to propagate
    // the offset information, but that is easier
    auto page = gnt->new_frame();
    memcpy(page, addr, size);

    auto ref = gnttab_claim_grant_reference(&_head);
    gnttab_grant_foreign_access_ref(ref, gnt->_otherend, virt_to_mfn(page), 0);
    return gntref(ref, page);
}

void *kernel_gntalloc::new_frame() {
    return aligned_alloc(4096, 4096);
}

gntref kernel_gntalloc::alloc_ref() {

    void *page = new_frame();
    uint32_t ref;

    if (gnttab_grant_foreign_access(_otherend, virt_to_mfn(page), 0, &ref)) {
        throw std::runtime_error("Failed to initialize allocate grant\n");
    }

    return gntref(int(ref), page);
}

grant_head *kernel_gntalloc::alloc_ref(unsigned nr_ents) {

    std::vector<gntref> v;
    uint32_t head;

    if (gnttab_alloc_grant_references(nr_ents, &head)) {
        throw std::runtime_error("Failed to initialize allocate grant\n");
    }

    return new kernel_grant_head(head);
}

void kernel_grant_head::free_ref(gntref& ref) {
    gnttab_end_foreign_access_ref(ref.xen_id);
    gnttab_release_grant_reference(&_head, ref.xen_id);
    free(ref.page);
    ref = invalid_ref;
}
#endif

gntalloc *gntalloc::_instance = nullptr;
gntalloc *gntalloc::instance(bool userspace, unsigned otherend) {

    if (!_instance) {
#ifdef HAVE_OSV
        if (!userspace) {
            _instance = new kernel_gntalloc(otherend);
        } else
#endif
            _instance = new userspace_gntalloc(otherend);
    }
    return _instance;
}

gntalloc *gntalloc::instance() {

    if (!_instance) {
        throw std::runtime_error("Acquiring grant instance without specifying otherend: invalid context");
    }
    return _instance;
}

}