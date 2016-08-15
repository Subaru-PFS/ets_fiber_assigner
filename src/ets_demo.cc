/*
 *  This file is part of ets_fiber_assigner.
 *
 *  ets_finer_assigner is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  ets_finer_assigner is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with ets_finer_assigner; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 *  ets_finer_assigner is being developed at the Max-Planck-Institut fuer
 *  Astrophysik.
 */

/*! \file ets_demo.cc */

#include <vector>
#include <set>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <memory>
#include <regex>

#include "error_handling.h"
#include "string_utils.h"
#include "geom_utils.h"
#include "lsconstants.h"
#include "pointing.h"
#include "paramfile.h"
#include "rotmatrix.h"

using namespace std;

/*! Simple class for storing a position in a 2D plane. */
class vec2
  {
  public:
    double x,y;

    vec2() {}
    vec2(double x_, double y_) : x(x_), y(y_) {}
    double dsq(const vec2 &b) const
      { return (x-b.x)*(x-b.x) + (y-b.y)*(y-b.y); }
  };

/*! Simple class containing all relevant properties of a PFS observation
    target. */
class Target
  {
  public:
    vec2 pos;
    double time;
    int pri;
    int id;

    Target (double x, double y, double time_, int id_, int pri_)
      : pos(x,y), time(time_), pri(pri_), id(id_) {}
  };

namespace {

/*! Priority queue that allows changing the priority of its entries after
    its creation
    Originally developed for Gadget 4. */
template <typename T, typename Compare=std::less<T>> class pqueue
  {
  private:
    Compare comp;
    struct node_t
      {
      T pri;
      size_t pos;
      };
    vector<node_t> nodes;
    vector<size_t> idx;

    void sift_up (size_t i)
      {
      size_t moving_node = idx[i];
      T moving_pri = nodes[moving_node].pri;

      for (size_t parent_node=i>>1;
           (i>1) && (comp(nodes[idx[parent_node]].pri,moving_pri));
           i=parent_node, parent_node=i>>1)
        {
        idx[i] = idx[parent_node];
        nodes[idx[i]].pos=i;
        }

      idx[i] = moving_node;
      nodes[idx[i]].pos=i;
      }

    size_t maxchild(size_t i) const
      {
      size_t child_node = i<<1;

      if (child_node>=idx.size())
        return 0;

      if (((child_node+1)<idx.size())
          && (comp(nodes[idx[child_node]].pri,nodes[idx[child_node+1]].pri)))
        child_node++; /* use right child instead of left */

      return child_node;
      }

    void sift_down(size_t i)
      {
      size_t moving_node = idx[i];
      T moving_pri = nodes[moving_node].pri;

      size_t child_node;
      while ((child_node = maxchild(i))
             && comp(moving_pri,nodes[idx[child_node]].pri))
        {
        idx[i] = idx[child_node];
        nodes[idx[i]].pos=i;
        i = child_node;
        }

      idx[i] = moving_node;
      nodes[idx[i]].pos=i;
      }

    /*! Rearranges the internal data structure to ensure the heap property. */
    void heapify()
      {
      size_t startnode=idx.size()>>1;
      for (size_t i=startnode; i>=1; --i)
        sift_down(i);
      }

  public:
    /*! Constructs a \a pqueue of size \a n. All priorities are set to zero. */
    pqueue (size_t n) : nodes(n), idx(n+1)
      {
      idx[0]=0;
      for (size_t i=0; i<n; ++i)
        {
        nodes[i]= {0.,i+1};
        idx[i+1]=i;
        }
      }
    /*! Constructs a \a pqueue with priorities taken from \a pri. */
    pqueue (const vector<T> &pri) : nodes(pri.size()), idx(pri.size()+1)
      {
      idx[0]=0;
      for (size_t i=0; i<pri.size(); ++i)
        {
        nodes[i]= {pri[i],i+1};
        idx[i+1]=i;
        }
      heapify();
      }

    /*! Sets the priority of the entry \a pos to \a new_pri. The heap is rebuilt
        automatically. */
    void set_priority(T new_pri, size_t pos)
      {
      T old_pri = nodes[pos].pri;
      nodes[pos].pri=new_pri;
      size_t posn = nodes[pos].pos;
      comp(old_pri,new_pri) ? sift_up(posn) : sift_down(posn);
      }

    /*! Returns the priority of the entry \a pos. */
    T priority(size_t pos) const
      { return nodes[pos].pri; }
    /*! Returns the lowest priority in the queue. */
    T top_priority() const
      { return nodes[idx[1]].pri; }
    /*! Returns entry with the lowest priority in the queue. */
    size_t top() const
      { return idx[1]; }
  };
constexpr size_t nfiber=3*57*14;
constexpr double rmax=4.75; // maximum radius of a fiber patrol area
constexpr double r_kernel=4.75; // radius of the priority function kernel
constexpr double dotdist=1.375; // radius of the dot blocking area
constexpr double colldist=2; // minimum distance between fiber positioners

/*! Class providing efficient queries for locations on a 2D plane */
class fpraster
  {
  private:
    double x0, y0, x1, y1, idx, idy;
    size_t nx, ny;
    vector<vector<size_t>> data;
    vector<vec2> loc;

    size_t indexx (double x) const
      { return size_t(max(0,min(int(nx)-1,int((x-x0)*idx)))); }
    size_t indexy (double y) const
      { return size_t(max(0,min(int(ny)-1,int((y-y0)*idy)))); }
    size_t index (const vec2 &pos) const
      { return indexx(pos.x) + nx*indexy(pos.y); }

  public:
    /*! Constructs an \a fpraster with \a nx_ bins in x direction and
        \a ny_ bins in y direction, and sorts the entries in \a loc_ into this
        structure. */
    fpraster (const vector<vec2> &loc_, size_t nx_, size_t ny_) :
      nx(nx_),ny(ny_),data(nx*ny), loc(loc_)
      {
      planck_assert ((nx>0) && (ny>0), "bad array sizes");
      planck_assert(loc.size()>0,"input array too small");
      x0=x1=loc[0].x;
      y0=y1=loc[0].y;
      for (size_t i=1; i<loc.size(); ++i)
        {
        x0=min(x0,loc[i].x); x1=max(x1,loc[i].x);
        y0=min(y0,loc[i].y); y1=max(y1,loc[i].y);
        }
      if (x0==x1) x1+=1e-9;
      if (y0==y1) y1+=1e-9;
      idx=nx/(x1-x0);
      idy=ny/(y1-y0);
      for (size_t i=0; i<loc.size(); ++i)
        data[index(loc[i])].push_back(i);
      }
    /*! Returns the indices of all \a loc entries that lie within a circle of
        radius \a rad around \a center. */
    vector<size_t> query(const vec2 &center, double rad) const
      {
      vector<size_t> res;
      if ((center.x<x0-rad)||(center.x>x1+rad)
        ||(center.y<y0-rad)||(center.y>y1+rad))
        return res;
      double rsq=rad*rad;
      size_t i0=indexx(center.x-rad), i1=indexx(center.x+rad),
            j0=indexy(center.y-rad), j1=indexy(center.y+rad);
      for (size_t j=j0; j<=j1; ++j)
        for (size_t i=i0; i<=i1; ++i)
          for (auto k : data[i+nx*j])
            if (center.dsq(loc[k])<=rsq) res.push_back(k);
      return res;
      }
    bool anyIn (const vec2 &center, double rad) const
      {
      if ((center.x<x0-rad)||(center.x>x1+rad)
        ||(center.y<y0-rad)||(center.y>y1+rad))
        return false;
      double rsq=rad*rad;
      size_t i0=indexx(center.x-rad), i1=indexx(center.x+rad),
             j0=indexy(center.y-rad), j1=indexy(center.y+rad);
      for (size_t j=j0; j<=j1; ++j)
        for (size_t i=i0; i<=i1; ++i)
          for (auto k : data[i+nx*j])
            if (center.dsq(loc[k])<=rsq) return true;
      return false;
      }
  };

/*! Converts RA/DEC in degrees to colatitude/longitude in radians. */
inline pointing radec2ptg (double ra, double dec)
  { return pointing((90-dec)*degr2rad,ra*degr2rad); }

void rotate (vec2 &pos, double sa, double ca)
  {
  vec2 t{pos};
  pos.x = ca*t.x - sa*t.y;
  pos.y = sa*t.x + ca*t.y;
  }

/*! Converts target coordinates from RA/DEC in degrees to PFI coordinates in
    millimeters, given a telescope pointing and orientation.
    \note This is still very preliminary, incomplete and approximate! */
void targetToPFI(vector<Target> &tgt, const pointing &los, double psi,
  double /*elevation*/)
  {
  vec3 z{los}, sky{0,0,1};
  vec3 x=(sky-z*dotprod(z,sky)).Norm();
  vec3 y=crossprod(z,x);
  double cpsi=cos(psi),spsi=sin(psi);
  const double a0=0., a1=-3.2e2, a2=-1.37e1, a3=-7.45e0;
  for (auto&& t:tgt)
    {
    vec3 pos=radec2ptg(t.pos.x,t.pos.y);
    vec3 xp=pos-y*dotprod(pos,y);
    vec3 yp=pos-x*dotprod(pos,x);
    vec2 pnew (atan2(dotprod(xp,x),dotprod(xp,z))*rad2degr,
               atan2(dotprod(yp,y),dotprod(yp,z))*rad2degr);
    rotate (pnew,cpsi,spsi);
    double rsq=pnew.x*pnew.x+pnew.y*pnew.y;
    t.pos.x= (a3*rsq*rsq+a2*rsq+a1)*pnew.x+a0;
    t.pos.y= (-a3*rsq*rsq-a2*rsq-a1)*pnew.y+a0;
    }
  }

/*! Computes the central fiber position in PFI coordinates, given the fiber ID.
    Fiber ID is zero-based throughout this code, i.e. ranging from 0 to 2393. */
vec2 id2fiberpos(int id)
  {
  int field=id/(57*14);
  id-=field*57*14;
  int module=id/57;
  int cobra=id-module*57;
  const double vspace=sqrt(0.75); // cos(30deg)
  vec2 res;
  res.y=0.5+module-0.5*cobra;
  res.x=-vspace*(1.+2*module+(cobra&1));
  if (field==1) rotate(res,-vspace,-0.5);
  if (field==2) rotate(res,vspace,-0.5);
  res.x*=8; res.y*=8;
  return res;
  }

/*! Computes the position of a dot center in PFI coordinates, given a fiber ID.
    Fiber ID is zero-based throughout this code, i.e. ranging from 0 to 2393. */
vec2 id2dotpos(int id) // id is assumed to be in [0; 2394[
  {
  vec2 res=id2fiberpos(id);
  res.y+=1.19;
  return res;
  }

/*! Remove a given value from a vector of integers. Assert that exactly one
    value was removed. */
inline void stripout (vector<size_t> &v, size_t val)
  {
  size_t s1=v.size();
  v.erase(remove(v.begin(),v.end(),val));
  planck_assert(v.size()+1==s1,"oops");
  }
fpraster tgt2raster (const vector<Target> &tgt, int nx, int ny)
  {
  vector<vec2> tpos(tgt.size());
  for (size_t i=0; i<tgt.size(); ++i) tpos[i]=tgt[i].pos;
  return fpraster (tpos,nx,ny);
  }

#if 0
/*! Diagnostic function to check for inconsistencies in the fiber->target and
    target->fiber mappings. */
void checkMappings (const vector<Target> &tgt,
  const vector<vector<size_t>> &/*f2t*/, const vector<vector<size_t>> &t2f)
  {
  vector<bool> tmp(tgt.size(), false);
  for (const auto &i : t2f)
    {
    planck_assert(i.size()<=3,"too many fibers");
    set<size_t> tmp2;
    for (auto j:i)
      {
      planck_assert(tmp2.find(j)==tmp2.end(),"multiply defined fiber");
      tmp2.insert(j);
      }
    }
  }
#endif

/*! Computes the fiber->target and target->fiber mappings. */
void calcMappings (const vector<Target> &tgt, const fpraster &raster,
  vector<vector<size_t>> &f2t, vector<vector<size_t>> &t2f)
  {
  f2t=vector<vector<size_t>>(nfiber);
  for (size_t i=0; i<nfiber; ++i)
    {
    vec2 fp=id2fiberpos(i), dp=id2dotpos(i);
    vector<size_t> tmp=raster.query(fp,rmax);
    for (auto j : tmp)
      if (dp.dsq(tgt[j].pos)>=dotdist*dotdist) f2t[i].push_back(j);
    }
  t2f=vector<vector<size_t>>(tgt.size());
  for (size_t i=0; i<f2t.size(); ++i)
    for (auto t : f2t[i])
      t2f[t].push_back(i);

 // for (auto &v: f2t) sort(v.begin(), v.end());
 // for (auto &v: t2f) sort(v.begin(), v.end());
  }

/*! Given a target index \a itgt and a fiber index \a fiber observing this
    target, remove all references to \a itgt from the mappings and also remove
    all targets that lie in the blocking area around \a itgt and all targets
    exclusively visible from \a fiber. */
void cleanup (const vector<Target> &tgt, const fpraster &raster,
  vector<vector<size_t>> &f2t, vector<vector<size_t>> &t2f, int fiber, int itgt)
  {
  // remove everything related to the selected fiber
  for (auto curtgt : f2t[fiber]) stripout(t2f[curtgt],fiber);
  f2t[fiber].clear();
  // remove target and everything in blocking area
  vector<size_t> tmp=raster.query(tgt[itgt].pos,colldist);
  for (auto i : tmp)
    {
    for (auto j : t2f[i]) stripout(f2t[j],i);
    t2f[i].clear();
    }
//  checkMappings(tgt,f2t,t2f);
  }

inline double kernelfunc(double rsq)
  {
  // simple parabola - quick but probably not optimal
  return max(0.,r_kernel*r_kernel-rsq);
//  return sqrt(max(0.,r_kernel*r_kernel-rsq)); // linear decrease
//  return exp(-9*rsq/(r_kernel*r_kernel)); // Gaussian kernel
  }

struct pq_entry
  {
  double prox;
  int pri;

  pq_entry() : prox(0.), pri(0) {}
  pq_entry(double prox_,int pri_) : prox(prox_), pri(pri_) {}

  bool operator< (const pq_entry &other) const
    {
    if (pri!=other.pri) return pri>other.pri;
    return prox<other.prox;
    }
  };

pqueue<pq_entry> calc_pri(const vector<Target> &tgt,
  const vector<vector<size_t>> &t2f, const fpraster &raster)
  {
  vector<pq_entry> pri(tgt.size());
  for (size_t i=0; i<tgt.size(); ++i)
    {
    if (t2f[i].size()>0)
      {
      vector<size_t> ngb = raster.query(tgt[i].pos,r_kernel);

      for (auto j : ngb)
        {
        if (i==j)
          pri[i].prox+=tgt[i].time*tgt[i].time*kernelfunc(0.);
        if (i<j)
          {
          double tmp=tgt[i].time*tgt[j].time
                    *kernelfunc(tgt[i].pos.dsq(tgt[j].pos));
          pri[i].prox+=tmp;
          pri[j].prox+=tmp;
          }
        }
      }
    }
  for (size_t i=0; i<tgt.size(); ++i)
    pri[i].pri=tgt[i].pri;
  pqueue<pq_entry> res(pri);
  return res;
  }

void fix_priority(const vector<Target> &tgt, const vector<vector<size_t>> &t2f,
  const fpraster &raster, size_t itgt, pqueue<pq_entry> &pri)
  {
  vector<size_t> ngb = raster.query(tgt[itgt].pos,r_kernel);
  for (auto j : ngb)
    if ((!t2f[j].empty())||(pri.priority(j).prox!=0.))
      {
      pq_entry tpri=pri.priority(j);
      tpri.prox-=tgt[j].time*tgt[itgt].time
                *kernelfunc(tgt[itgt].pos.dsq(tgt[j].pos));
      pri.set_priority(tpri,j);
      }
  }

class FiberAssigner
  {
  public:
    /*! Assign target from \a tgt to fibers according to a given strategy.
        On return, \a tid and \a fid contain target resp. fiber IDs for the
        assigned targets. Target IDs range from 0 to \a tgt.size()-1, fiber
        IDs from 0 to 2393. */
    virtual void assign (const vector<Target> &tgt,
      vector<size_t> &tid, vector<size_t> &fid) const = 0;

    virtual ~FiberAssigner() {}
  };

int maxpri_in_fiber (size_t fiber, const vector<Target> &tgt,
  const vector<vector<size_t>> &f2t)
  {
  planck_assert(!f2t[fiber].empty(), "searching in empty fiber");
  size_t idx=0;
  int maxpri = tgt[f2t[fiber][idx]].pri;
  for (size_t j=1; j<f2t[fiber].size(); ++j)
    if (tgt[f2t[fiber][idx]].pri<maxpri)
      { maxpri=tgt[f2t[fiber][idx]].pri; idx=j; }
  return f2t[fiber][idx];
  }

class NaiveAssigner: public FiberAssigner
  {
  /*! Naive assignment algorithm: iterate over all fibers, and if a fiber has
      targets in its patrol area, assign the target with the highest priority
      to it. */
  virtual void assign (const vector<Target> &tgt,
    vector<size_t> &tid, vector<size_t> &fid) const
    {
    tid.clear(); fid.clear();
    fpraster raster=tgt2raster(tgt,100,100);
    vector<vector<size_t>> f2t,t2f;
    calcMappings(tgt,raster,f2t,t2f);

    for (size_t fiber=0; fiber<f2t.size(); ++fiber)
      {
      if (f2t[fiber].empty()) continue;
      int itgt = maxpri_in_fiber(fiber,tgt,f2t);
      tid.push_back(itgt);
      fid.push_back(fiber);
      cleanup (tgt, raster, f2t, t2f, fiber, itgt);
      }
    }
  };

class DrainingAssigner: public FiberAssigner
  {
  /*! Assignment strategy modeled after Morales et al. 2012: MNRAS 419, 1187
      find the fiber(s) with the smallest number of observable targets >0;
      for the first of the returned fibers, assign the target with highest
      priority to it; repeat until no more targets are observable. */
  virtual void assign (const vector<Target> &tgt,
    vector<size_t> &tid, vector<size_t> &fid) const
    {
    tid.clear(); fid.clear();
    fpraster raster=tgt2raster(tgt,100,100);
    vector<vector<size_t>> f2t,t2f;
    calcMappings(tgt,raster,f2t,t2f);

    size_t maxtgt=0;
    for (const auto &f:f2t)
      maxtgt=max(maxtgt,f.size());

    while (true)
      {
      int fiber=-1;
      size_t mintgt=maxtgt+1;
      for (size_t i=0; i<f2t.size(); ++i)
        if ((f2t[i].size()<mintgt)&&(f2t[i].size()>0))
          { fiber=i; mintgt=f2t[i].size(); }
      if (fiber==-1) break; // assignment done
      int itgt = maxpri_in_fiber(fiber,tgt,f2t);
      tid.push_back(itgt);
      fid.push_back(fiber);
      cleanup(tgt,raster,f2t,t2f,fiber,itgt);
      }
    }
  };

class NewAssigner: public FiberAssigner
  {
  /*! Assignment strategy with the goal of reducing inhomogeneity in the
      target distribution: assign a priority to each target that depends on
      the distance of all other targets in its close vicinity; process targets
      in order of decreasing priority and assign them to fibers, if possible.
      After each assignment, update the priority of the remaining targets. */
  virtual void assign (const vector<Target> &tgt,
    vector<size_t> &tid, vector<size_t> &fid) const
    {
    tid.clear(); fid.clear();
    fpraster raster=tgt2raster(tgt,100,100);
    vector<vector<size_t>> f2t,t2f;
    calcMappings(tgt,raster,f2t,t2f);
    pqueue<pq_entry> pri=calc_pri(tgt,t2f,raster);

    while (true)
      {
      if (pri.top_priority().pri==(1<<30)) break;
      size_t itgt=pri.top();
      if (t2f[itgt].empty())
        { pri.set_priority(pq_entry(0.,(1<<30)),itgt); continue; }
      size_t ifib=0, mintgt=f2t[t2f[itgt][ifib]].size();
      for (size_t i=1; i<t2f[itgt].size(); ++i)
        if (f2t[t2f[itgt][i]].size()<mintgt)
          { ifib=i; mintgt=f2t[t2f[itgt][i]].size(); }
      int fiber=t2f[itgt][ifib];
      tid.push_back(itgt);
      fid.push_back(fiber);
      cleanup(tgt,raster,f2t,t2f,fiber,itgt);
      fix_priority(tgt,t2f,raster,itgt,pri);
      }
    }
};

/*! Discard targets that are too far away from the PFS. */
vector<size_t> select_observable (const vector<Target> &tgt, double safety)
  {
  vector<vec2> fpos(nfiber);
  for (size_t i=0; i<fpos.size(); ++i) fpos[i]=id2fiberpos(i);
  fpraster raster(fpos,100,100);
  vector<size_t> res;
  for (size_t i=0; i<tgt.size(); ++i)
    if (raster.anyIn(tgt[i].pos,rmax+safety))
      res.push_back(i);
  return res;
  }

void single_exposure(const vector<Target> &tgt, const pointing &center,
  double posang, double elevation, const FiberAssigner &ass,
  vector<size_t> &tid, vector<size_t> &fid)
  {
  tid.clear(); fid.clear();
  vector<Target> tgt1(tgt);
  targetToPFI(tgt1, center, posang, elevation);
#if 1
  vector<size_t> idx = select_observable (tgt1, r_kernel);
  vector<Target> tgt2;
  for (auto i:idx)
    tgt2.push_back(tgt1[i]);
  if (!tgt2.empty()) ass.assign(tgt2,tid,fid);
  for (size_t i=0; i<tid.size(); ++i)
    tid[i]=idx[tid[i]];
#else
  if (!tgt1.empty()) ass.assign(tgt1,tid,fid);
#endif
  }

} // unnamed namespace

void optimal_exposure(const vector<Target> &tgt, pointing &center, double dptg,
  int nptg, double &posang, double dposang, int nposang, double elevation,
  const FiberAssigner &ass, vector<size_t> &tid, vector<size_t> &fid)
  {
  double posang0=posang;
  tid.clear(); fid.clear();
  vec3 vcenter(center);
  vec3 vdx=crossprod(vcenter,vec3(0,0,1));
  if (vdx.SquaredLength()==0.) // center lies at a pole
    vdx=vec3(1,0,0);
  else
    vdx.Normalize();
  vec3 vdy=crossprod(vcenter,vdx);
  //FIXME: make this user-definable!
  for (int idx=0; idx<nptg; ++idx)
    for (int idy=0; idy<nptg; ++idy)
      for (int ida=0; ida<nposang; ++ida)
        {
        double dx=-dptg+2*dptg*(idx+0.5)/nptg;
        double dy=-dptg+2*dptg*(idy+0.5)/nptg;
        double da=-dposang+2*dposang*(ida+0.5)/nposang;
        pointing newcenter(vcenter+(vdx*dx+vdy*dy));
        double newposang=posang0+da;
        vector<size_t> tid2,fid2;
        single_exposure (tgt, newcenter, newposang, elevation, ass, tid2, fid2);
        if (tid2.size()>tid.size())
          { tid=tid2; fid=fid2; center=newcenter; posang=newposang; }
        }
  }

namespace {

void strip (vector<Target> &tgt, const vector<size_t> &remove, double time)
  {
  vector<bool> obs(tgt.size(),false);
  for (size_t i=0; i<remove.size(); ++i)
    obs[remove[i]]=true;
  vector<Target> t2;
  for (size_t i=0; i<obs.size(); ++i)
    if (!obs[i])
      t2.push_back(tgt[i]);
    else
      if (tgt[i].time>time+1e-7)
        {
        t2.push_back(tgt[i]);
        t2.back().time-=time;
        }
  tgt.swap(t2);
  }

template<typename T> string toString(const T&val, int w)
  {
  ostringstream o;
  o<<setw(w)<<val;
  return o.str();
  }
template<typename T> string toString(const T&val, int w, int p)
  {
  ostringstream o;
  o<<fixed<<setw(w)<<setprecision(p)<<val;
  return o.str();
  }

void subprocess (const vector<Target> &tgt, const pointing &center0,
  double dptg, int nptg, double posang0, double dposang, int nposang,
  double elevation, double fract, ofstream &fout, const FiberAssigner &ass)
  {
  vector<Target> tgt1=tgt;
  double ttime=0., acc=0., time2=0.;
  for (const auto &t: tgt)
    ttime += t.time;
  cout << endl << "Total observation time: " << ttime << endl;
  size_t cnt=0;
  cout << endl << "tile # | fiber allocation fraction | "
                  "total observation fraction | time"
       << endl;
  while (true)
    {
    pointing center(center0);
    double posang(posang0);
    vector<size_t> tidmax, fidmax;
    optimal_exposure(tgt1, center, dptg, nptg, posang, dposang, nposang,
      elevation, ass, tidmax, fidmax);
    if (tidmax.empty()) break; // stop if no more fibers could be assigned
    double time=tgt1[tidmax[0]].time;
    for (const auto i: tidmax)
      if (tgt1[i].time<time) time=tgt1[i].time;
    time2+=time;
    acc+=tidmax.size()*time;
    if (fout.is_open())
      {
      fout << "Exposure " << cnt << ": duration " << time << "s, "
        "RA: " << rad2degr*center.phi << ", DEC " << 90-rad2degr*center.theta
        << " PA: " << rad2degr*posang << endl
        << "  Target     Fiber        RA       DEC" << endl;
      //FIXME: add PFI coordinates
      for (size_t i=0; i<tidmax.size(); ++i)
        fout << toString(tgt1[tidmax[i]].id,8) << toString(fidmax[i]+1,10)
        << toString(tgt1[tidmax[i]].pos.x,10,5)
        << toString(tgt1[tidmax[i]].pos.y,10,5)
        << endl;
      }
    cout << toString(cnt++,6)
         << toString(tidmax.size()/double(nfiber),18,5)
         << toString(acc/ttime,28,5)
         << toString(time2,20,0) << endl;
    cout << toString(rad2degr*center.phi,12,8) << " "
         << toString(90-rad2degr*center.theta,12,8) << " "
         << toString(posang*rad2degr,12,8) << endl;
    if (acc/ttime>fract) break;
    strip (tgt1,tidmax,time);
    }
  }

/*! Reads targets from the ASCII file \a name and returns them in a \a vector.
    The returned coordinates are RA/DEC in degrees. */
vector<Target> readTargets (const string &name)
  {
  int lineno=0;
  vector<Target> res;
  ifstream inp(name);
  planck_assert (inp,"Could not open target file '"+name+"'.");
  while (inp)
    {
    string line;
    getline(inp, line);
    ++lineno;
    // remove potential carriage returns at the end of the line
    line=line.substr(0,line.find("\r"));
    line=line.substr(0,line.find("#"));
    line=trim(line);
    if (line.size()>0)
      {
      istringstream iss(line);
      double x,y,time;
      string id0;
      int pri;
      iss >> id0 >> x >> y >> time >> pri;
      if (iss)
        {
        planck_assert((id0.length()>2) && (id0.substr(0,2)=="ID"),
          "identifier not starting with 'ID'");
        int id=stringToData<int>(id0.substr(2));
        res.emplace_back(x,y,time,id,pri);
        }
      else
        cerr << "Warning: unrecognized format in '" << name << "', line "
             << lineno << ":\n" << line << endl;
      }
    }
  return res;
  }

void process(const string &name, double fract,
  const pointing &center, double dptg, int nptg, double posang, double dposang,
  int nposang, const string &out,
  const FiberAssigner &ass)
  {
  double elevation=0; /*ignored for the moment */
  vector<Target> tgt=readTargets(name);
  {
  vector<Target> tmp(tgt), tgt2;
  targetToPFI(tmp, center, posang, elevation);
  for (size_t i=0; i<tmp.size(); ++i)
    if (tmp[i].pos.dsq(vec2(0,0))<190*190)
      tgt2.push_back(tgt[i]);
  tgt.swap(tgt2);
  }
  ofstream fout;
  if (out!="")
    { fout.open(out); planck_assert(fout,"error opening output file"); }
  subprocess (tgt, center, dptg, nptg, posang, dposang, nposang, elevation,
    fract, fout, ass);
  }

/*! Finds the smallest circle enclosing all locations in \a tgt and returns
    its center. Used to find a telescope pointing that hits the given target
    list. Only for temporary use. */
vec3 getCenter(const vector<Target> &tgt)
  {
  vector<vec3> tmp;
  for (auto t:tgt)
    tmp.push_back(vec3(radec2ptg(t.pos.x,t.pos.y)));
  double dummy;
  vec3 res;
  find_enclosing_circle(tmp,res,dummy);
  pointing pcnt(res);
  cout << "center of data set: RA " << rad2degr*pcnt.phi
       << ", DEC " << 90-rad2degr*pcnt.theta << endl;
  return res;
  }

#if 1
double greg2julian (int y, int m, int d)
  {
  if (m<=2) // Jan, Feb
    { --y; m+=12; }
  int a=y/100;
  int b=a/4;
  int c=2-a+b;
  int e=365.25*(y+4716);
  int f=30.6001*(m+1);
  return c+d+e+f-1524.5;
  }

void julian2greg (double jd, int *year, int *month, int *day)
  {
  double q=jd+0.5;
  int z=(int) q;
  int w=(z-1867216.25)/36524.25;
  int x=w/4;
  int a=z+1+w-x;
  int b=a+1524;
  int c=(b-122.1)/365.25;
  int d=365.25*c;
  int e=(b-d)/30.6001;
  int f=30.6001*e;
  *day=b-d-f+(q-z);
  *month=e-1; if (*month>12) month-=12;
  *year=c-4716; if (*month<=2) (*year)++;
  }

double jd2gmst (double jd)
  {
  double jd0=(int)(jd+0.5)-0.5;
  double h=(jd-jd0)*24;
  double d=jd-2451545.0;
  double d0=jd0-2451545.0;
  double t=d/36525.;
  double res = 6.697374558 + 0.06570982441908*d0 + 1.00273790935*h + 0.000026*t*t;
  return fmodulo(res,24.);
  }

double jd2gast (double jd)
  {
  double gmst=jd2gmst(jd);
  double d=jd-2451545.0;
  double omega=125.04-0.052954*d;
  double l = 280.47 + 0.98565*d;
  double eps = 23.4393 - 0.0000004*d;
  double dpsi=-0.000319*sin(omega*degr2rad) - 0.000024*sin(2*l*degr2rad);
  double res=gmst+dpsi*cos(eps*degr2rad);
  return fmodulo(res,24.);
  }
double jd2gmst_approx (double jd)
  {
  double res = 18.697374558 + 24.06570982441908*(jd-2451545.0);
  return fmodulo(res,24.);
  }

double gmst2ha (double gmst, double lon, double ra) // time in h, angles in rad
  {
  return fmodulo(gmst*15*degr2rad+lon-ra, twopi);
  }
double iso8601toJD (const std::string &datetime)
  {
  regex reg_date(R"foo(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z$)foo");
  std::smatch match;
  planck_assert(regex_search(datetime,match,reg_date),"unknown date format");
  planck_assert(match.size()==7,"unexpected number of matches");
  double jd0=greg2julian(stringToData<int>(match[1]),
                         stringToData<int>(match[2]),
                         stringToData<int>(match[3]));
  jd0 += stringToData<int>(match[4])/24. + 
         stringToData<int>(match[5])/(24.*60.) + 
         stringToData<double>(match[6])/(24.*60.*60.);
  return jd0;
  }
template<size_t n> double poly (double x, const array<double, n> &c)
  {
  double res=0., v=1.;
  for (size_t i=0; i<c.size(); ++i)
    {
    res+=c[i]*v;
    v*=x;
    }
  return res;
  }

void nutate (double jd, double &ra, double &dec)
  {
  //  form time in Julian centuries from 1900.0 Hmmm? Looks tather like 2000.0
  double t = (jd - 2451545.)/36525.;

  // Mean elongation of the Moon
  const array<double,4> coeff1 { 297.85036,  445267.111480, -0.0019142, 1./189474 };
  double d=fmodulo(poly(t,coeff1)*degr2rad,twopi);

  // Sun's mean anomaly
  const array<double,4> coeff2 { 357.52772, 35999.050340, -0.0001603, -1./3e5 };
  double m=fmodulo(poly(t,coeff2)*degr2rad,twopi);

  // Moon's mean anomaly
  const array<double,4> coeff3 { 134.96298, 477198.867398, 0.0086972, 1./5.625e4 };
  double mprime = fmodulo(poly(t,coeff3)*degr2rad,twopi);

  // Moon's argument of latitude
  const array<double,4> coeff4 { 93.27191, 483202.017538, -0.0036825, -1./3.27270e5 };
  double f = fmodulo(poly(t,coeff4)*degr2rad,twopi);

  // Longitude of the ascending node of the Moon's mean orbit on the ecliptic,
  // measured from the mean equinox of the date

  const array<double,4> coeff5 { 125.04452, -1934.136261, 0.0020708, 1./4.5e5 };
  double omega = fmodulo(poly(t,coeff5)*degr2rad,twopi);

  const array<double,63> d_lng { 0,-2,0,0,0,0,-2,0,0,-2,-2,-2,0,2,0,2,0,0,-2,0,2,0,0,-2,0,-2,0,0,2,
   -2,0,-2,0,0,2,2,0,-2,0,2,2,-2,-2,2,2,0,-2,-2,0,-2,-2,0,-1,-2,1,0,0,-1,0,0,
     2,0,2};
  const array<double,63> m_lng
 {0,0,0,0,1,0,1,0,0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,2,1,0,-1,0,0,0,1,1,-1,0,
  0,0,0,0,0,-1,-1,0,0,0,1,0,0,1,0,0,0,-1,1,-1,-1,0,-1};
  const array<double,63> mp_lng{0,0,0,0,0,1,0,0,1,0,1,0,-1,0,1,-1,-1,1,2,-2,0,2,2,1,0,0,-1,0,-1,
   0,0,1,0,2,-1,1,0,1,0,0,1,2,1,-2,0,1,0,0,2,2,0,1,1,0,0,1,-2,1,1,1,-1,3,0 };
  const array<double,63> f_lng {0,2,2,0,0,0,2,2,2,2,0,2,2,0,0,2,0,2,0,2,2,2,0,2,2,2,2,0,0,2,0,0,
   0,-2,2,2,2,0,2,2,0,2,2,0,0,0,2,0,2,0,2,-2,0,0,0,2,2,0,0,2,2,2,2 };
  const array<double,63> om_lng {1,2,2,2,0,0,2,1,2,2,0,1,2,0,1,2,1,1,0,1,2,2,0,2,0,0,1,0,1,2,1,
   1,1,0,1,2,2,0,2,1,0,2,1,1,1,0,1,1,1,1,1,0,0,0,0,0,2,0,0,2,2,2,2 };
  const array<double,63> sin_lng {-171996, -13187, -2274, 2062, 1426, 712, -517, -386, -301, 217,
    -158, 129, 123, 63, 63, -59, -58, -51, 48, 46, -38, -31, 29, 29, 26, -22,
     21, 17, 16, -16, -15, -13, -12, 11, -10, -8, 7, -7, -7, -7,
     6,6,6,-6,-6,5,-5,-5,-5,4,4,4,-4,-4,-4,3,-3,-3,-3,-3,-3,-3,-3};
  const array<double,63> sdelt {-174.2, -1.6, -0.2, 0.2, -3.4, 0.1, 1.2, -0.4, 0, -0.5, 0, 0.1,
     0,0,0.1, 0,-0.1,0,0,0,0,0,0,0,0,0,0, -0.1, 0, 0.1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
  const array<double,63> cos_lng { 92025, 5736, 977, -895, 54, -7, 224, 200, 129, -95,0,-70,-53,0,
    -33, 26, 32, 27, 0, -24, 16,13,0,-12,0,0,-10,0,-8,7,9,7,6,0,5,3,-3,0,3,3,
     0,-3,-3,3,3,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
  const array<double,63> cdelt {8.9, -3.1, -0.5, 0.5, -0.1, 0.0, -0.6, 0.0, -0.1, 0.3,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

  double d_psi=0., d_eps=0.;
  // Sum the periodic terms
  for (size_t n=0; n<d_lng.size(); ++n)
    {
    double arg=d_lng[n]*d + m_lng[n]*m +mp_lng[n]*mprime + f_lng[n]*f +om_lng[n]*omega;
    double sarg = sin(arg);
    double carg = cos(arg);
    d_psi += 0.0001*(sdelt[n]*t + sin_lng[n])*sarg;
    d_eps += 0.0001*(cdelt[n]*t + cos_lng[n])*carg;
    }

  double eps0 = 23.4392911*3600. - 46.8150*t - 0.00059*t*t + 0.001813*t*t*t;
  double eps = (eps0 + d_eps)/3600.*degr2rad; // true obliquity of the ecliptic in radians

  double ce = cos(eps);
  double se = sin(eps);

  // convert ra-dec to equatorial rectangular coordinates
  vec3 p1(pointing(halfpi-dec,ra));
const double d2as=pi/(180.*3600.);
  // apply corrections to each rectangular coordinate
  vec3 p2 (p1.x - (p1.y*ce + p1.z*se)*d_psi * d2as,
           p1.y + (p1.x*ce*d_psi - p1.z*d_eps) * d2as,
           p1.z + (p1.x*se*d_psi + p1.y*d_eps) * d2as);
  pointing pp2(p2);
  dec=halfpi-pp2.theta;
  ra=pp2.phi;
  }

void precess (double &ra, double &dec, double equinox1, double equinox2)
  {
  const double sec2rad=degr2rad/3600.;
  vec3 x(pointing(halfpi-dec, ra));
  double t = 1e-3*(equinox2-equinox1);
  double st = 1e-3*(equinox1-2000.);
  double A=sec2rad*t*(23062.181 + st*(139.656 +0.0139*st)
    + t*(30.188 - 0.344*st+17.998*t));
  double B=sec2rad*t*t*(79.280 + 0.410*st + 0.205*t) + A;
  double C=sec2rad*t*(20043.109 - st*(85.33 + 0.217*st)
    + t*(-42.665 - 0.217*st -41.833*t));

  double sina = sin(A), sinb = sin(B), sinc = sin(C),
         cosa = cos(A), cosb = cos(B), cosc = cos(C);

  rotmatrix r(
    vec3( cosa*cosb*cosc-sina*sinb,sina*cosb+cosa*sinb*cosc, cosa*sinc),
    vec3(-cosa*sinb-sina*cosb*cosc,cosa*cosb-sina*sinb*cosc,-sina*sinc),
    vec3(-cosb*sinc, -sinb*sinc, cosc));

  vec3 x2 = r.Transform(x); //rotate to get output direction cosines

  pointing ptg(x2);
  ra = ptg.phi;
  ra += (ra<0.)*twopi;
  dec= halfpi-ptg.theta;
  }

void transformtest ()
  {
  const double j2000= 2451545.0;
  double jd=iso8601toJD("2016-11-01T08:53:01Z");
  cout << "jd="<<dataToString(jd)<<endl;

  double lat=(19+49/60.+32/3600.)*degr2rad; //Subaru
  double lon=-(155+28/60.+34/3600.)*degr2rad; //Subaru
  double ra=34.0*degr2rad;
  double decl=-4.5*degr2rad;
  double gmst=jd2gast(jd);
  cout << dataToString(ra) << " " << dataToString(decl) << endl;
  cout << ra*rad2degr << " " << decl*rad2degr << endl;
  precess(ra, decl, 2000., 2000. + (jd-j2000) / 365.25);
  cout << dataToString(ra) << " " << dataToString(decl) << endl;
  cout << ra*rad2degr << " " << decl*rad2degr << endl;
 // nutate(jd,ra,decl);
  cout << dataToString(ra) << " " << dataToString(decl) << endl;
  cout <<  (jd-j2000) / 365.25 << endl;
  cout << ra*rad2degr << " " << decl*rad2degr << endl;
  double ha=gmst2ha (gmst,lon,ra);
  cout << "hour angle [hours]: " << ha*rad2degr/15-24 << " " << dataToString(ha*rad2degr/15) << endl;
  double alt=asin(sin(decl)*sin(lat)+cos(decl)*cos(lat)*cos(ha));
  double az=acos((sin(decl)-sin(alt)*sin(lat))/(cos(alt)*cos(lat)));
  if (sin(ha)>0) az=twopi-az;
  cout << dataToString(alt*rad2degr) << endl;
  cout << dataToString(az*rad2degr) << endl;
  }
#endif
} // unnamed namespace

int main(int argc, const char ** argv)
  {
    transformtest();
  map<string,string> paramdict;
  parse_cmdline_equalsign (argc, argv, paramdict);
  paramfile params (paramdict);

  unique_ptr<FiberAssigner> pass;
  string assignerName=params.find<string>("assigner");
  if (assignerName=="naive")
    pass=make_unique<NaiveAssigner>();
  else if (assignerName=="draining")
    pass=make_unique<DrainingAssigner>();
  else if (assignerName=="new")
    pass=make_unique<NewAssigner>();
  else
    planck_fail("unknown assigner");
  pointing center;
  if (params.param_present("ra")||params.param_present("dec"))
    center=radec2ptg (params.find<double>("ra"), params.find<double>("dec"));
  else
    center=pointing(getCenter(readTargets(params.find<string>("input"))));

  double posang=degr2rad*params.find<double>("posang",0.);
  double dposang=degr2rad*params.find<double>("dposang",4.);
  int nposang=params.find<int>("nposang",5);
  double dptg=degr2rad*params.find<double>("dptg",4./320.);// should roughly correspond to 4mm in PFI plane
  int nptg=params.find<int>("nptg",5);
  process (params.find<string>("input"),
    params.find<double>("fract"),center,dptg,nptg,posang,dposang,nposang,
    params.find<string>("output",""),*pass);
  }
