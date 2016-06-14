#include "gvt/render/unit/DomainTracer.h"

#include <pthread.h>
#include <iostream>
#include <cstring>

#include "gvt/render/unit/Communicator.h"

#include "gvt/render/unit/CommonWorks.h"
#include "gvt/render/unit/DomainWorks.h"
#include "gvt/render/unit/TpcVoter.h"

#include "gvt/render/actor/Ray.h"

#include <gvt/core/mpi/Wrapper.h>
#include <gvt/render/Types.h>
#ifdef GVT_USE_MPE
#include "mpe.h"
#endif
#include <gvt/render/RenderContext.h>
#include <gvt/render/Schedulers.h>
#include <gvt/render/Types.h>
#include <gvt/render/algorithm/TracerBase.h>

#ifdef GVT_RENDER_ADAPTER_EMBREE
#include <gvt/render/adapter/embree/Wrapper.h>
#endif

#ifdef GVT_RENDER_ADAPTER_MANTA
#include <gvt/render/adapter/manta/Wrapper.h>
#endif
#ifdef GVT_RENDER_ADAPTER_OPTIX
#include <gvt/render/adapter/optix/Wrapper.h>
#endif
#if defined(GVT_RENDER_ADAPTER_OPTIX) && defined(GVT_RENDER_ADAPTER_EMBREE)
#include <gvt/render/adapter/heterogeneous/Wrapper.h>
#endif

#include <boost/foreach.hpp>

#include <set>

#define DEBUG_TX

namespace gvt {
namespace render {
namespace unit {

using namespace gvt::render::actor;
using namespace gvt::render::data::scene;

gvt::core::time::timer t_send(false, "domain tracer: send :");
gvt::core::time::timer t_recv(false, "domain tracer: recv :");
gvt::core::time::timer t_vote(false, "domain tracer: vote :");

DomainTracer::DomainTracer(const MpiInfo &mpiInfo, Worker *worker,
                           Communicator *comm, RayVector &rays,
                           gvt::render::data::scene::Image &image)
    : RayTracer(mpiInfo, worker, comm), AbstractTrace(rays, image) {
  voter = NULL;
  if (mpiInfo.size > 1) voter = new TpcVoter(mpiInfo, *this, comm, worker);

  pthread_mutex_init(&workQ_mutex, NULL);

#ifdef GVT_USE_MPE
  // MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPE_Log_get_state_eventIDs(&tracestart, &traceend);
  MPE_Log_get_state_eventIDs(&shufflestart, &shuffleend);
  MPE_Log_get_state_eventIDs(&framebufferstart, &framebufferend);
  MPE_Log_get_state_eventIDs(&localrayfilterstart, &localrayfilterend);
  MPE_Log_get_state_eventIDs(&intersectbvhstart, &intersectbvhend);
  MPE_Log_get_state_eventIDs(&marchinstart, &marchinend);
  if (mpiInfo.rank == 0) {
    MPE_Describe_state(tracestart, traceend, "Process Queue", "blue");
    MPE_Describe_state(shufflestart, shuffleend, "Shuffle Rays", "green");
    MPE_Describe_state(framebufferstart, framebufferend, "Gather Framebuffer",
                       "orange");
    MPE_Describe_state(localrayfilterstart, localrayfilterend,
                       "Filter Rays Local", "coral");
    MPE_Describe_state(intersectbvhstart, intersectbvhend, "Intersect BVH",
                       "azure");
    MPE_Describe_state(marchinstart, marchinend, "March Ray in", "LimeGreen");
  }
#endif
  gvt::core::Vector<gvt::core::DBNodeH> dataNodes =
      rootnode["Data"].getChildren();

  // create a map of instances to mpi rank
  for (size_t i = 0; i < instancenodes.size(); i++) {
    gvt::core::DBNodeH meshNode = instancenodes[i]["meshRef"].deRef();

    size_t dataIdx = -1;
    for (size_t d = 0; d < dataNodes.size(); d++) {
      if (dataNodes[d].UUID() == meshNode.UUID()) {
        dataIdx = d;
        break;
      }
    }

    // NOTE: mpi-data(domain) assignment strategy
    size_t mpiNode = dataIdx % mpiInfo.size;

    GVT_DEBUG(DBG_ALWAYS, "[" << mpiInfo.rank << "] domain scheduler: instId: " << i
                              << ", dataIdx: " << dataIdx
                              << ", target mpi node: " << mpiNode
                              << ", world size: " << mpiInfo.size);

    GVT_ASSERT(dataIdx != -1, "domain scheduler: could not find data node");
    mpiInstanceMap[i] = mpiNode;
  }
}

void DomainTracer::Render() {
  // TODO: set these variables only once in the constructor
  Trace();

  if (mpiInfo.rank == 0) {
    Work *work = new Command(Command::QUIT);
    work->SendAll(comm);
  }
}

void DomainTracer::shuffleDropRays(gvt::render::actor::RayVector &rays) {
  const size_t chunksize =
      MAX(2, rays.size() / (std::thread::hardware_concurrency() * 4));
  static gvt::render::data::accel::BVH &acc =
      *dynamic_cast<gvt::render::data::accel::BVH *>(acceleration);
  static tbb::simple_partitioner ap;
  tbb::parallel_for(
      tbb::blocked_range<gvt::render::actor::RayVector::iterator>(
          rays.begin(), rays.end(), chunksize),
      [&](tbb::blocked_range<gvt::render::actor::RayVector::iterator> raysit) {
        std::vector<gvt::render::data::accel::BVH::hit> hits =
            acc.intersect<GVT_SIMD_WIDTH>(raysit.begin(), raysit.end(), -1);
        std::map<int, gvt::render::actor::RayVector> local_queue;
        for (size_t i = 0; i < hits.size(); i++) {
          gvt::render::actor::Ray &r = *(raysit.begin() + i);
          if (hits[i].next != -1) {
            r.origin = r.origin + r.direction * (hits[i].t * 0.8f);
            const bool inRank = mpiInstanceMap[hits[i].next] == mpiInfo.rank;
            if (inRank) local_queue[hits[i].next].push_back(r);
          }
        }
        for (auto &q : local_queue) {
          queue_mutex[q.first].lock();
          queue[q.first].insert(
              queue[q.first].end(),
              std::make_move_iterator(local_queue[q.first].begin()),
              std::make_move_iterator(local_queue[q.first].end()));
          queue_mutex[q.first].unlock();
        }
      },
      ap);

  rays.clear();
}

inline void DomainTracer::FilterRaysLocally() { shuffleDropRays(rays); }

inline void DomainTracer::Trace() {
  gvt::core::time::timer t_diff(false, "domain tracer: diff timers/frame:");
  gvt::core::time::timer t_all(false, "domain tracer: all timers:");
  gvt::core::time::timer t_frame(true, "domain tracer: frame :");
  gvt::core::time::timer t_gather(false, "domain tracer: gather :");
  // gvt::core::time::timer t_send(false, "domain tracer: send :");
  gvt::core::time::timer t_shuffle(false, "domain tracer: shuffle :");
  gvt::core::time::timer t_trace(false, "domain tracer: trace :");
  gvt::core::time::timer t_sort(false, "domain tracer: select :");
  gvt::core::time::timer t_adapter(false, "domain tracer: adapter :");
  gvt::core::time::timer t_filter(false, "domain tracer: filter :");

  // gvt::core::time::timer t_trace(false);
  // gvt::core::time::timer t_sort(false);
  // gvt::core::time::timer t_shuffle(false);
  // gvt::core::time::timer t_gather(false);
  // gvt::core::time::timer t_send(false);
  // gvt::core::time::timer t_frame(true);
  GVT_DEBUG(DBG_ALWAYS,
            "domain scheduler: starting, num rays: " << rays.size());
  gvt::core::DBNodeH root =
      gvt::render::RenderContext::instance()->getRootNode();

  clearBuffer();
  int adapterType = root["Schedule"]["adapter"].value().toInteger();

  long domain_counter = 0;

// FindNeighbors();

// sort rays into queues
// note: right now throws away rays that do not hit any domain owned by the
// current
// rank
#ifdef GVT_USE_MPE
  MPE_Log_event(localrayfilterstart, 0, NULL);
#endif
  t_filter.resume();
  FilterRaysLocally();
  t_filter.stop();
#ifdef GVT_USE_MPE
  MPE_Log_event(localrayfilterend, 0, NULL);
#endif

  GVT_DEBUG(DBG_LOW, "tracing rays");

  // process domains until all rays are terminated
  bool all_done = false;
  int nqueue = 0;
  std::set<int> doms_to_send;
  int lastInstance = -1;
  // gvt::render::data::domain::AbstractDomain* dom = NULL;

  gvt::render::actor::RayVector moved_rays;
  moved_rays.reserve(1000);

  int instTarget = -1;
  size_t instTargetCount = 0;

  gvt::render::Adapter *adapter = 0;
  while (!all_done) {
    // process domain with most rays queued
    instTarget = -1;
    instTargetCount = 0;

    t_sort.resume();
    GVT_DEBUG(DBG_ALWAYS,
              "image scheduler: selecting next instance, num queues: "
                  << this->queue.size());
    // for (std::map<int, gvt::render::actor::RayVector>::iterator q =
    // this->queue.begin(); q != this->queue.end();
    //      ++q) {
    for (auto &q : queue) {
      const bool inRank = mpiInstanceMap[q.first] == mpiInfo.rank;
      if (inRank && q.second.size() > instTargetCount) {
        instTargetCount = q.second.size();
        instTarget = q.first;
      }
    }
    t_sort.stop();
    GVT_DEBUG(DBG_ALWAYS, "image scheduler: next instance: "
                              << instTarget << ", rays: " << instTargetCount);

    if (instTarget >= 0) {
      t_adapter.resume();
      gvt::render::Adapter *adapter = 0;
      // gvt::core::DBNodeH meshNode =
      // instancenodes[instTarget]["meshRef"].deRef();

      gvt::render::data::primitives::Mesh *mesh = meshRef[instTarget];

      // TODO: Make cache generic needs to accept any kind of adpater

      // 'getAdapterFromCache' functionality
      auto it = adapterCache.find(mesh);
      if (it != adapterCache.end()) {
        adapter = it->second;
      } else {
        adapter = 0;
      }
      if (!adapter) {
        GVT_DEBUG(DBG_ALWAYS, "image scheduler: creating new adapter");
        switch (adapterType) {
#ifdef GVT_RENDER_ADAPTER_EMBREE
          case gvt::render::adapter::Embree:
            adapter =
                new gvt::render::adapter::embree::data::EmbreeMeshAdapter(mesh);
            break;
#endif
#ifdef GVT_RENDER_ADAPTER_MANTA
          case gvt::render::adapter::Manta:
            adapter =
                new gvt::render::adapter::manta::data::MantaMeshAdapter(mesh);
            break;
#endif
#ifdef GVT_RENDER_ADAPTER_OPTIX
          case gvt::render::adapter::Optix:
            adapter =
                new gvt::render::adapter::optix::data::OptixMeshAdapter(mesh);
            break;
#endif

#if defined(GVT_RENDER_ADAPTER_OPTIX) && defined(GVT_RENDER_ADAPTER_EMBREE)
          case gvt::render::adapter::Heterogeneous:
            adapter = new gvt::render::adapter::heterogeneous::data::
                HeterogeneousMeshAdapter(mesh);
            break;
#endif
          default:
            GVT_DEBUG(DBG_SEVERE,
                      "image scheduler: unknown adapter type: " << adapterType);
        }

        adapterCache[mesh] = adapter;
      }
      t_adapter.stop();
      GVT_ASSERT(adapter != nullptr, "image scheduler: adapter not set");
      // end getAdapterFromCache concept

      GVT_DEBUG(DBG_ALWAYS, "image scheduler: calling process queue");
      {
        t_trace.resume();
        moved_rays.reserve(this->queue[instTarget].size() * 10);
#ifdef GVT_USE_DEBUG
        boost::timer::auto_cpu_timer t("Tracing rays in adapter: %w\n");
#endif
        adapter->trace(this->queue[instTarget], moved_rays, instM[instTarget],
                       instMinv[instTarget], instMinvN[instTarget], lights);

        this->queue[instTarget].clear();

        t_trace.stop();
      }

      GVT_DEBUG(DBG_ALWAYS, "image scheduler: marching rays");
      t_shuffle.resume();
      shuffleRays(moved_rays, instTarget);
      moved_rays.clear();
      t_shuffle.stop();
    }
    all_done = TransferRays();
  }

// std::cout << "domain scheduler: select time: " << t_sort.format();
// std::cout << "domain scheduler: trace time: " << t_trace.format();
// std::cout << "domain scheduler: shuffle time: " << t_shuffle.format();
// std::cout << "domain scheduler: send time: " << t_send.format();

// add colors to the framebuffer
#ifdef GVT_USE_MPE
  MPE_Log_event(framebufferstart, 0, NULL);
#endif
  t_gather.resume();
  //this->gatherFramebuffers(this->rays_end - this->rays_start);
  CompositeFrameBuffers();
  t_gather.stop();
#ifdef GVT_USE_MPE
  MPE_Log_event(framebufferend, 0, NULL);
#endif
  t_frame.stop();
  t_all = t_sort + t_trace + t_shuffle + t_gather + t_adapter + t_filter +
          t_send + t_recv + t_vote;
  t_diff = t_frame - t_all;
}

bool DomainTracer::TransferRays() {
  bool done;
  if (mpiInfo.size > 1) {
    if (voter->isCommunicationAllowed()) {  // TODO: potential improvement
      t_send.resume();
      SendRays();
      t_send.stop();
      // profiler.update(Profiler::Send, t_send.getElapsed());

      t_recv.resume();
      RecvRays();
      t_recv.stop();
      // profiler.update(Profiler::Receive, t_receive.getElapsed());
    }

    t_vote.resume();
    done = voter->updateState();
    t_vote.stop();
#ifdef DEBUG_VOTER
    if (mpiInfo.rank == 0)
      printf("rank %d: voter state %d\n", mpiInfo.rank, voter->state);
#endif
    // profiler.update(Profiler::Vote, t_vote.getElapsed());

  } else {
    done = IsDone();
  }
  // assert(!done || (done && !hasWork()));
  assert(!done || (done && IsDone()));
  return done;
}

void DomainTracer::SendRays() {
#ifdef PROFILE_RAY_COUNTS
  uint64_t ray_count = 0;
#endif
  for (auto &q : queue) {
    int instance = q.first;
    RayVector &rays = q.second;
    int owner_process = mpiInstanceMap[instance];
    size_t num_rays_to_send = rays.size();

    if (owner_process != mpiInfo.rank && num_rays_to_send > 0) {
      voter->addNumPendingRays(num_rays_to_send);

      RemoteRays::Header header;
      header.transfer_type = RemoteRays::Request;
      header.sender = mpiInfo.rank;
      header.instance = instance;
      header.num_rays = num_rays_to_send;

      RemoteRays *work = new RemoteRays(header, rays);
      work->Send(owner_process, comm);
      // RemoteRays *work = new RemoteRays(RemoteRays::getSize(
      //     num_rays_to_send * sizeof(gvt::render::actor::Ray)));
      // work->setup(RemoteRays::Request, rank, instance, rays);
      // work.Send(owner_process);
      // SendWork(work, owner_process);

      rays.clear();
#ifdef PROFILE_RAY_COUNTS
      ray_count += num_rays_to_send;
#endif
#ifdef DEBUG_TX
      printf("rank %d: sent %lu rays instance %d to rank %d\n", mpiInfo.rank,
             num_rays_to_send, instance, owner_process);
#endif
    }
  }
#ifdef PROFILE_RAY_COUNTS
  profiler.addRayCountSend(ray_count);
#endif
}

inline void DomainTracer::BufferWork(Work *work) {
  pthread_mutex_lock(&workQ_mutex);
  workQ.push(work);
  pthread_mutex_unlock(&workQ_mutex);
}

void DomainTracer::RecvRays() {
#ifdef PROFILE_RAY_COUNTS
  uint64_t ray_count = 0;
#endif
  pthread_mutex_lock(&workQ_mutex);
  while (!workQ.empty()) {
    RemoteRays *rays = static_cast<RemoteRays *>(workQ.front());

    CopyRays(*rays);

    RemoteRays::Header header;
    header.transfer_type = RemoteRays::Grant;
    header.sender = mpiInfo.rank;
    header.instance = rays->GetInstance();
    header.num_rays = rays->GetNumRays();

    RemoteRays *grant = new RemoteRays(header);
    grant->Send(rays->GetSender(), comm);
#ifdef PROFILE_RAY_COUNTS
    ray_count += rays->getNumRays();
#endif
#ifdef DEBUG_TX
    printf("rank %d: recved %d rays instance %d \n", mpiInfo.rank,
           rays->GetNumRays(), rays->GetInstance());
#endif
    workQ.pop();
    delete rays;
  }
  pthread_mutex_unlock(&workQ_mutex);

#ifdef PROFILE_RAY_COUNTS
  profiler.addRayCountRecv(ray_count);
#endif
}

void DomainTracer::CopyRays(const RemoteRays &rays) {
  int instance = rays.GetInstance();
  int num_rays = rays.GetNumRays();

  const Ray *begin = reinterpret_cast<const Ray *>(rays.GetRayBuffer());
  const Ray *end = begin + rays.GetNumRays();

#ifdef DEBUG_TX
  printf("ray copy begin %p end %p instance %d num_rays %d\n", begin, end, instance, num_rays);
#endif

  if (queue.find(instance) != queue.end()) {
    RayVector &r = queue[instance];
    r.insert(r.end(), begin, end);
  } else {
    queue[instance] = RayVector();
    RayVector &r = queue[instance];
    r.insert(r.end(), begin, end);
  }
}

void DomainTracer::LocalComposite() {
  const size_t size = width * height;
  const size_t chunksize =
      MAX(2, size / (std::thread::hardware_concurrency() * 4));
  static tbb::simple_partitioner ap;
  tbb::parallel_for(tbb::blocked_range<size_t>(0, size, chunksize),
                    [&](tbb::blocked_range<size_t> chunk) {
                      for (size_t i = chunk.begin(); i < chunk.end(); i++)
                        image.Add(i, colorBuf[i]);
                    },
                    ap);
}

void DomainTracer::CompositeFrameBuffers() {
#ifdef DEBUG_TX
  printf("start of %s\n", __PRETTY_FUNCTION__);
#endif
  LocalComposite();
  // for (size_t i = 0; i < size; i++) image.Add(i, colorBuf[i]);

  // if (!mpiInfo) return;

  size_t size = width * height;
  unsigned char *rgb = image.GetBuffer();

  int rgb_buf_size = 3 * size;

  unsigned char *bufs =
      (mpiInfo.rank == 0) ? new unsigned char[mpiInfo.size * rgb_buf_size] : NULL;

  // MPI_Barrier(MPI_COMM_WORLD);
  MPI_Gather(rgb, rgb_buf_size, MPI_UNSIGNED_CHAR, bufs, rgb_buf_size,
             MPI_UNSIGNED_CHAR, 0, MPI_COMM_WORLD);
  if (mpiInfo.rank == 0) {
    const size_t chunksize =
        MAX(2, size / (std::thread::hardware_concurrency() * 4));
    static tbb::simple_partitioner ap;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, size, chunksize),
                      [&](tbb::blocked_range<size_t> chunk) {

                        for (int j = chunk.begin() * 3; j < chunk.end() * 3;
                             j += 3) {
                          for (size_t i = 1; i < mpiInfo.size; ++i) {
                            int p = i * rgb_buf_size + j;
                            // assumes black background, so adding is fine
                            // (r==g==b== 0)
                            rgb[j + 0] += bufs[p + 0];
                            rgb[j + 1] += bufs[p + 1];
                            rgb[j + 2] += bufs[p + 2];
                          }
                        }
                      });
  }
  delete[] bufs;
}

}  // namespace unit
}  // namespace render
}  // namespace gvt

