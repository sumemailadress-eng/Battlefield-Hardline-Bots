// Positions are from the dedicated level's open prompts and wired button events.
typedef struct MapDoor {
    float gate[3],control[3],radius,hold,nx,nz;
    int remote;
    ULONGLONG busyUntil;
} MapDoor;
static MapDoor mapDoors[1024];
static int mapDoorCount,doorIndexLoaded;
static unsigned doorGridSerial;
static int indexed_panel(const float *hit)
{
    for (int k=0;k<mapDoorCount;k++) {
        MapDoor *d=&mapDoors[k];
        float dx=hit[0]-d->gate[0],dz=hit[2]-d->gate[2];
        if (fabsf(hit[1]-d->gate[1])<1.3f && dx*dx+dz*dz<2.25f &&
            fabsf(dx*d->nx+dz*d->nz)<0.65f) return 1;
    }
    return 0;
}
static void door_index_reset(void)
{
    nav_clear_fixed_walls();mapDoorCount=doorIndexLoaded=0;doorGridSerial=0;
}
static void door_index_tick(void)
{
    if (!g_levelKey[0]) return;
    if (!doorIndexLoaded) {
        char key[80],path[160],line[256];
        map_key(key,sizeof key);snprintf(path,sizeof path,"Thugbot_doors\\%s.doors",key);
        FILE *f=fopen(path,"r");doorIndexLoaded=1;
        if (!f) return;
        int walls=0,controls=0;
        while (fgets(line,sizeof line,f)) {
            char *record=line;
            if (*record=='@') {
                char *space=strchr(record,' ');const char *mode=strrchr(g_levelKey,'_');
                if (!space || !mode) continue;
                *space=0;if (_stricmp(record+1,mode+1)) continue;
                record=space+1;
            }
            float ax,az,bx,bz,low,high;
            if (sscanf(record,"wall %f %f %f %f %f %f",&ax,&az,&bx,&bz,&low,&high)==6) {
                nav_add_fixed_wall(ax,az,bx,bz,low,high);walls++;continue;
            }
            MapDoor d={0};char kind[12];
            if (sscanf(record,"%11s %f %f %f %f %f %f %f %f %f %f",kind,
                d.gate,d.gate+1,d.gate+2,d.control,d.control+1,d.control+2,&d.radius,&d.hold,&d.nx,&d.nz)!=11) continue;
            d.remote=!strcmp(kind,"button");
            if ((!d.remote && strcmp(kind,"direct") && strcmp(kind,"double")) || mapDoorCount>=1024 ||
                !(d.radius>0 && d.radius<=12 && d.hold>=0 && d.hold<=5)) continue;
            mapDoors[mapDoorCount++]=d;controls+=d.remote;
        }
        fclose(f);
        H->log("door index: %s, %d open prompts, %d wired control links, %d fixed bar spans",key,mapDoorCount-controls,controls,walls);
    }
    if (g_nav && !g_navBuilding && doorGridSerial!=g_nav->serial) {
        doorGridSerial=g_nav->serial;
        int cuts=nav_cut_fixed_walls(g_nav);
        if(cuts)H->log("door index: removed %d grid crossings through fixed bars",cuts);
    }
}

static int door_floor(const float *at,float y,float *floor)
{
    float a[3]={at[0],y+0.9f,at[2]},b[3]={at[0],y-1.0f,at[2]},normal[3];
    return ray_cast(a,b,floor,normal) && normal[1]>0.65f;
}

// Approach the control from accessible ground on this side. The actual switch can be on a wall.
static int control_stance(const MapDoor *d,const float *p,float *stance)
{
    float distance=dist2d(p,d->control);
    float eyeY=p[1]+1.15f;
    float budget=d->radius*d->radius-(eyeY-d->control[1])*(eyeY-d->control[1]);
    if (budget<=0) return 0;
    float stand=fminf(1.15f,sqrtf(budget)*0.78f);
    if (distance<=stand) { memcpy(stance,p,12);return 1; }
    float u=(distance-stand)/distance;
    float towards[3]={p[0]+u*(d->control[0]-p[0]),p[1],p[2]+u*(d->control[2]-p[2])};
    return door_floor(towards,p[1],stance) && foot_connector(p,stance);
}

// A switch around a corner may require a short foot route. Test candidate
// stances at interaction height and let the normal planner verify the walk.
static int routed_control_stance(const MapDoor *d,const float *p,float *stance)
{
    float yaw=atan2f(p[0]-d->control[0],p[2]-d->control[2]),best=1e9f;
    for(int j=0;j<8;j++) {
        float angle=yaw+j*0.785398f;
        float ring=fminf(1.2f,d->radius*0.7f);
        float q[3]={d->control[0]+sinf(angle)*ring,p[1],d->control[2]+cosf(angle)*ring},floor[3];
        if(!door_floor(q,p[1],floor))continue;
        float eye[3]={floor[0],floor[1]+1.15f,floor[2]},hit[3];
        if (ring*ring+(eye[1]-d->control[1])*(eye[1]-d->control[1])>d->radius*d->radius) continue;
        if (ray_cast(eye,d->control,hit,NULL) && dist2d(hit,d->control)>0.3f) continue;
        float score=dist2d(p,floor);
        if (score<best) {best=score;memcpy(stance,floor,12);}
    }
    return best<1e9f;
}

static int select_map_door(Bot *b,const float *p,const float *aim,ULONGLONG t)
{
    float dx=aim[0]-p[0],dz=aim[2]-p[2],len=hypotf(dx,dz);
    if (len<0.35f) return 0;
    dx/=len;dz/=len;
    float best=1e9f;int chosen=-1,routed=0;
    for(int k=0;k<mapDoorCount;k++) {
        MapDoor *d=&mapDoors[k];
        if (d->busyUntil>t || fabsf(d->gate[1]-(p[1]+1.15f))>1.55f) continue;
        float x=d->gate[0]-p[0],z=d->gate[2]-p[2],along=x*dx+z*dz;
        if(along<0.25f || along>fminf(4.5f,len+0.65f) || fabsf(z*dx-x*dz)>(d->remote?1.85f:1.0f)) continue;
        float nx=d->nx,nz=d->nz;
        if (nx*nx+nz*nz<0.5f) {nx=dx;nz=dz;}
        if(nx*x+nz*z<0) {nx=-nx;nz=-nz;}
        if(nx*dx+nz*dz<0.35f) continue;
        float nearPt[3]={d->gate[0]-0.9f*nx,p[1],d->gate[2]-0.9f*nz};
        float farPt[3]={d->gate[0]+0.95f*nx,p[1],d->gate[2]+0.95f*nz};
        float nearFloor[3],farFloor[3],stance[3];
        if (!door_floor(nearPt,p[1],nearFloor) || !door_floor(farPt,p[1],farFloor) ||
            nav_fixed_wall_blocks(nearFloor,farFloor) || foot_connector(nearFloor,farFloor)) continue;
        // A wall beside a door must not trigger the door. Check the approach on our side.
        if (!foot_connector(p,nearFloor)) continue;
        int direct=control_stance(d,p,stance);
        if (!direct && (!d->remote || !g_nav || g_navBuilding || !routed_control_stance(d,p,stance))) continue;
        float score=dist2d(p,stance)+fabsf(z*dx-x*dz)+along+(direct?0:4);
        if(score>=best)continue;
        best=score;chosen=k;routed=!direct;memcpy(b->doorNear,nearFloor,12);memcpy(b->doorFar,farFloor,12);
        memcpy(b->doorStance,stance,12);
    }
    if(chosen<0)return 0;
    b->doorReturning=0;b->doorRouted=routed;b->doorPathN=b->doorWp=0;
    b->doorId=chosen+1;b->doorUntil=t+(routed?16000:8500);b->doorPressed=0;b->doorCheck=0;
    // Both sides or a shared switch on two leaves must not issue competing pulses.
    for(int k=0;k<mapDoorCount;k++) {
        if ((dist2d(mapDoors[k].gate,mapDoors[chosen].gate)<0.35f && fabsf(mapDoors[k].gate[1]-mapDoors[chosen].gate[1])<1.6f) ||
            (dist2d(mapDoors[k].control,mapDoors[chosen].control)<0.25f && fabsf(mapDoors[k].control[1]-mapDoors[chosen].control[1])<0.5f))
            mapDoors[k].busyUntil=b->doorUntil;
    }
    return 1;
}

static int mapped_door_tick(Bot *b,void *soldier,const float *p,const float *aim,ULONGLONG t)
{
    if (!b->doorId) {
        if (t<b->doorCheck || !mapDoorCount) return 0;
        b->doorCheck=t+350+b->seed%100;
        if(!select_map_door(b,p,aim,t))return 0;
    }
    MapDoor *d=&mapDoors[b->doorId-1];
    if(t>=b->doorUntil) {
        b->doorId=0;b->doorCheck=t+1500;b->npts=b->wp=0;b->planAt=0;
        d->busyUntil=t+3500;return 0;
    }
    if (d->busyUntil<t+500) {
        for(int k=0;k<mapDoorCount;k++) {
            if ((dist2d(mapDoors[k].gate,d->gate)<0.35f && fabsf(mapDoors[k].gate[1]-d->gate[1])<1.6f) ||
                (dist2d(mapDoors[k].control,d->control)<0.25f && fabsf(mapDoors[k].control[1]-d->control[1])<0.5f))
                mapDoors[k].busyUntil=t+1200;
        }
    }
    b->buttons|=BTN_USE_RELEASE;b->progressAt=t;memcpy(b->progressPos,p,12);
    if(t>=b->doorCheck) {
        b->doorCheck=t+160;
        if(foot_connector(b->doorNear,b->doorFar)) {
            if(d->remote && !b->doorReturning) {
                b->doorReturning=1;b->doorUntil=t+13000;b->doorPathN=b->doorWp=0;
                memcpy(b->doorStance,b->doorNear,12);
                b->doorRouted=!foot_connector(p,b->doorNear);
                H->log("door index: %s linked passage clear; returning from switch",(char*)b->player+OFF_PLAYER_NAME);
            }
            if (!b->doorReturning || dist2d(p,b->doorNear)<0.75f) {
            memcpy(b->openingNear,b->doorNear,12);memcpy(b->openingFar,b->doorFar,12);
            b->openingIn=0;b->openingUntil=t+6500;b->doorId=0;
            H->log("door index: %s passage clear at %.1f %.1f %.1f after %s",(char*)b->player+OFF_PLAYER_NAME,
                   d->gate[0],d->gate[1],d->gate[2],b->doorPressed?(d->remote?"linked button":"open prompt"):"external opening");
            return 0;
            }
        } else if(b->doorReturning) {
            b->doorUntil=t;return 1;
        }
    }
    b->why=d->remote?"walking to door button":"lining up at door";
    b->fwd=b->strafe=b->turn=0;
    if(b->doorReturning)b->why="returning to open door";
    if((b->doorReturning || !b->doorPressed) && dist2d(p,b->doorStance)>0.35f) {
        const float *goal=b->doorStance;
        if(b->doorRouted) {
            if(!g_nav || g_navBuilding){b->doorUntil=t;return 1;}
            if(!b->doorPathN) {
                int owner=(int)(b-g_bot);
                nav_set_path_owner(owner);
                b->doorPathN=nav_find_path(g_nav,ray_cast,p[0],p[1],p[2],goal[0],goal[1],goal[2],b->doorPath,16);
                int deferred=nav_last_deferred();
                nav_set_path_owner(-1);
                if(!b->doorPathN){if(!deferred)b->doorUntil=t;return 1;}
                if(!foot_connector(b->doorPath+3*(b->doorPathN-1),goal)){b->doorUntil=t;return 1;}
            }
            while(b->doorWp<b->doorPathN && dist2d(p,b->doorPath+3*b->doorWp)<0.75f &&
                  fabsf(p[1]-b->doorPath[3*b->doorWp+1])<0.8f) b->doorWp++;
            if(b->doorWp<b->doorPathN) goal=b->doorPath+3*b->doorWp;
        }
        steer_towards(b,soldier,goal);return 1;
    }
    if(b->doorReturning)return 1;
    float dx=d->control[0]-p[0],dz=d->control[2]-p[2];
    float dy=d->control[1]-(p[1]+1.15f),want=atan2f(dx,dz);
    if(dx*dx+dz*dz+dy*dy>d->radius*d->radius) {
        if(!control_stance(d,p,b->doorStance))b->doorUntil=t;
        return 1;
    }
    b->turn=turn_input(soldier_yaw(soldier),want,3);
    if(fabsf(wrap_pi(want-soldier_yaw(soldier)))>0.36f && !b->doorPressed) return 1;
    if(!b->doorPressed) {
        float eye[3]={p[0],p[1]+1.15f,p[2]}, hit[3];
        if (ray_cast(eye,d->control,hit,NULL) && dist2d(hit,d->control)>0.30f) {
            b->doorUntil=t;return 1;
        }
        H->log("door index: %s operating %s at %.1f %.1f %.1f for passage %.1f %.1f %.1f",
               (char*)b->player+OFF_PLAYER_NAME,d->remote?"wired button":"open prompt",
               d->control[0],d->control[1],d->control[2],d->gate[0],d->gate[1],d->gate[2]);
        b->doorPressed=t;b->doorUntil=t+(ULONGLONG)(d->hold*1000)+(d->remote?12000:3200);}
    if(t-b->doorPressed<(ULONGLONG)(d->hold*1000)+180) {
        b->buttons&=~BTN_USE_RELEASE;b->buttons|=BTN_INTERACT;
    }
    b->why=d->remote?"using door button":"using door open prompt";
    return 1;
}
