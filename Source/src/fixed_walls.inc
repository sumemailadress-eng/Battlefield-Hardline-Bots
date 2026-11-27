// Fixed map collision spans, in metres. Clip the swept soldier against the
// wall length, thickness and height. Lintels remain passable underneath.
typedef struct FixedWall {
    float x,z,dx,dz,length,low,high;
} FixedWall;
static FixedWall fixedWalls[128];
static int fixedCount;
void nav_clear_fixed_walls(void) { fixedCount=0; }

void nav_add_fixed_wall(float x,float z,float endX,float endZ,float low,float high)
{
    float dx=endX-x,dz=endZ-z,length=hypotf(dx,dz);
    if (fixedCount>=128 || length<0.1f || length>50 || high<=low) return;
    FixedWall w={x,z,dx/length,dz/length,length,low,high};
    fixedWalls[fixedCount++]=w;
}

static int clip_wall_axis(float start,float delta,float lo,float hi,float *first,float *last)
{
    if (fabsf(delta)<0.00001f) return start>=lo && start<=hi;
    float a=(lo-start)/delta,b=(hi-start)/delta;
    *first=fmaxf(*first,fminf(a,b)); *last=fminf(*last,fmaxf(a,b));
    return *last>=*first;
}
static int hits_fixed_wall(const FixedWall *w,const float *a,const float *b)
{
    float x=a[0]-w->x,z=a[2]-w->z,dx=b[0]-a[0],dz=b[2]-a[2],first=0,last=1;
    return clip_wall_axis(a[1],b[1]-a[1],w->low-1.67f,w->high-0.10f,&first,&last) &&
        clip_wall_axis(x*w->dx+z*w->dz,dx*w->dx+dz*w->dz,-0.19f,w->length+0.19f,&first,&last) &&
        clip_wall_axis(z*w->dx-x*w->dz,dz*w->dx-dx*w->dz,-0.25f,0.25f,&first,&last);
}
int nav_fixed_wall_blocks(const float *a,const float *b)
{
    for (int k=0;k<fixedCount;k++) if (hits_fixed_wall(&fixedWalls[k],a,b)) return 1;
    return 0;
}

int nav_cut_fixed_walls(NavGrid *g)
{
    int removed=0;
    for (int k=0;k<fixedCount;k++) {
        const FixedWall *w=&fixedWalls[k];
        float ex=w->x+w->dx*w->length,ez=w->z+w->dz*w->length;
        int xmin=(int)floorf((fminf(w->x,ex)-g->x0)/g->cell)-2;
        int zmin=(int)floorf((fminf(w->z,ez)-g->z0)/g->cell)-2;
        int xmax=(int)ceilf((fmaxf(w->x,ex)-g->x0)/g->cell)+2;
        int zmax=(int)ceilf((fmaxf(w->z,ez)-g->z0)/g->cell)+2;
        for (int z=zmin<0?0:zmin;z<=zmax && z<g->h;z++)
        for (int x=xmin<0?0:xmin;x<=xmax && x<g->w;x++)
        for (int l=0;l<L;l++) {
            int n=(z*g->w+x)*L+l;
            if (!(g->flags[n]&NAV_WALKABLE)) continue;
            float a[3];node_pos(g,n,a);
            for (int d=0;d<8;d++) {
                int next=step_node(g,n,d);
                int fence=!(d&1) && (g->fences[n]&(1<<d));
                if (next<0 && fence) next=fence_step(g,n,d);
                if (next<0) continue;
                float b[3];node_pos(g,next,b);
                if (!hits_fixed_wall(w,a,b)) continue;
                if (g->edges[n]&(1<<d)) { g->edges[n]&=~(1<<d);removed++; }
                if (!(d&1)) { if(fence)removed++; g->fences[n]&=~(3<<d); }
            }
        }
    }
    return removed;
}
