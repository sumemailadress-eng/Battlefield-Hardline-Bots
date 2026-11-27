"""Build navigation hints from Hardline dedicated-server level bundles.
Usage: py door_index.py SERVER_DIRECTORY OUTPUT_DIRECTORY --reader PATH_TO_FROSTBITE_SCRIPTS/frostbite3 [--client CLIENT_DATA_DIRECTORY] [--map mp_bank]
The offline reader is https://github.com/NicknineTheEagle/Frostbite-Scripts .
Only placement coordinates, interaction distances and collision spans are written.
"""
import argparse
import math
from pathlib import Path
import re
import struct
import sys
from tempfile import TemporaryDirectory
import uuid

from fb_bundle import toc, bundles
from fb_minimap import Store, decompress, skip_blocks

def entries(path):
    data = path.with_suffix('.sb').read_bytes()
    for bundle in toc(str(path))['bundles']:
        base = bundle['offset']
        meta, magic, total, ne, nr, _, names, _, _ = struct.unpack_from('>9I', data, base)
        if magic != 0x9d798ed5:
            raise ValueError(f'unsupported dedicated-server bundle in {path}')
        table, strings, pos = base + 36 + 20 * total, base + 4 + names, base + 4 + meta
        for i in range(ne + nr):
            key, size = struct.unpack_from('>II', data, table + 8*i)
            offset = strings + key
            name = data[offset:data.index(0, offset)].decode().lower()
            end = skip_blocks(data, pos, size)
            yield i < ne, name, memoryview(data)[pos:end]
            pos = end

def fields(obj):
    for f in obj.fields:
        if f.desc.name == '$':
            yield from fields(f.value)
        else:
            yield f.desc.name, f.value

def field(obj, key, default=None):
    return next((v for k,v in fields(obj) if k == key), default)

def vec(obj):
    return tuple(field(obj,k) for k in ('x','y','z'))

def transform(obj):
    return [vec(field(obj,k)) for k in ('right','up','forward','trans')]

def world(matrix, point):
    return tuple(matrix[3][i]+sum(point[j]*matrix[j][i] for j in range(3)) for i in range(3))

def guid(raw):
    return str(uuid.UUID(bytes_le=bytes(raw)))

def numbers(values):
    if not all(math.isfinite(v) for v in values):
        raise ValueError('invalid world coordinate')
    return ' '.join(f'{v:.3f}' for v in values)

def collision_spans(group, raw, asset_name):
    # Each member's PhysicsPartRange identifies its placements in the cooked compound.
    bounds, parts = {}, {}
    for _,obj in group.instances:
        if obj.desc.name == 'GroupHavokAsset':
            for a, box in zip(field(obj,'ExternalAssets').fields, field(obj,'Aabb').fields):
                name = asset_name(group,a.value)
                if not re.search(r'/metalbarsvault_\w+_st$',name): continue
                part = field(box.value,'PartAabb').fields
                if len(part) == 1:
                    bounds[name] = (vec(field(part[0].value,'min')),vec(field(part[0].value,'max')))
        if obj.desc.name == 'StaticModelGroupEntityData':
            for m in field(obj,'MemberDatas').fields:
                name = asset_name(group,field(m.value,'MemberType'))
                if not re.search(r'/metalbarsvault_\w+_st$',name): continue
                span = field(m.value,'PhysicsPartRange')
                a,b = field(span,'First'),field(span,'Last')
                if b-a+1 == field(m.value,'InstanceCount'):
                    parts.update((i,name) for i in range(a,b+1))
    if not parts: return
    seen = {}
    for off in range(0,len(raw)-63,16):
        flags,tag = struct.unpack_from('<II',raw,off+56)
        index = tag & 0xffffff
        if flags != 0xffffffff or index not in parts or struct.unpack_from('<I',raw,off+44)[0] != 0x3f000000: continue
        tx,ty,tz,_,qx,qy,qz,qw,sx,sy,sz = struct.unpack_from('<11f',raw,off)
        if not all(map(math.isfinite,(tx,ty,tz,qx,qy,qz,qw,sx,sy,sz))):continue
        if abs(qx)+abs(qz)>0.02 or abs(qy*qy+qw*qw-1)>0.002 or min(sx,sy,sz)<=0:continue
        seen.setdefault(index,set()).add((tx,ty,tz,qy,qw,sx,sy,sz))
    for index,name in parts.items():
        placements = seen.get(index,set())
        if len(placements)!=1 or name not in bounds:
            raise ValueError(f'unresolved fixed bar placement {name} {index}')
        x,y,z,q,w,sx,sy,sz = next(iter(placements))
        lo,hi=bounds[name]
        mid=(lo[0]+hi[0])*0.5*sx
        def end(u):
            return (x+(1-2*q*q)*mid+2*w*q*u*sz,z-2*w*q*mid+(1-2*q*q)*u*sz)
        yield 'wall '+numbers((*end(lo[2]),*end(hi[2]),y+sy*lo[1],y+sy*hi[1]))

def normal(xf, parts, point):
    # Hinge-to-latch direction (two hinge positions for a double door).
    if len(parts)==2: a,b=parts
    elif len(parts)==1 and point is not None:a,b=parts[0],point
    else:return (0.0,0.0)
    a,b=world(xf,a),world(xf,b)
    dx,dz=b[0]-a[0],b[2]-a[2]
    length=math.hypot(dx,dz)
    return (-dz/length,dx/length) if length>0.45 else (0.0,0.0)

def mesh_bounds(store, mapname, wanted):
    if not store or not wanted:return {}
    result={}
    for path in store.tocs.get(mapname,[]):
        for _,bundle in bundles(path):
            for entry in bundle.get('res',[]):
                name=entry['name']
                if name not in wanted or 'sha1' not in entry or entry['sha1'] not in store.cat:continue
                raw=store.read(entry['sha1'])
                if len(raw)<32:continue
                bounds=struct.unpack_from('<8f',raw)
                if all(math.isfinite(bounds[k]) and 0<=bounds[k+4]-bounds[k]<100 for k in range(3)):
                    result[name]=bounds
    return result

def scan(path, tmp, ebx, store):
    objects, resources, names, deps = {}, {}, {}, {}
    interact=set()
    for is_ebx,name,buf in entries(path):
        if not is_ebx:
            if '/staticmodelgroup_physics_' in name:resources[name]=buf
            continue
        if name in objects: continue
        un,h = struct.unpack_from('>II',buf)
        size = un if h>>16 in (0x70,0x71) else h&65535
        head=decompress(buf[:8+size])
        if head[:4] not in (b'\xce\xd1\xb2\x0f',b'\xce\xd1\xb4\x0f'): continue
        count=struct.unpack_from('<I',head,12)[0]
        table_end=64+32*count+struct.unpack_from('<H',head,26)[0]
        if table_end>len(head):head=decompress(buf)
        if b'GameInteractionEntityData\0' in head[64+32*count:table_end]:interact.add(name)
        names[guid(head[40:56])]=name
        deps[name]={guid(head[n:n+16]) for n in range(64,64+count*32,32)}
        objects[name]=buf
    def read(name):
        tmp.write_bytes(decompress(objects[name]))
        return ebx.Dbx(str(tmp),str(tmp.parent))
    def ref(db,idx):
        if idx is None or not idx & 0x80000000: return ''
        return names.get(db.externalGUIDs[idx&0x7fffffff][0].format(),'')
    doors,buttons,leaves,shapes={}, {}, {}, {}
    for name in objects:
        short=name.split('/')[-1]
        if name not in interact and not (('door' in short or 'gate' in short or 'interactbutton' in short) and 'prefab' in short or 'interactbutton_pf' in short):continue
        db=read(name)
        if db.prim.desc.name in ('WorldPartData','SubWorldData','LevelData'):continue
        for _,obj in db.instances:
            if obj.desc.name=='GameInteractionEntityData':
                sid=field(obj,'InteractionSid','')
                hold=max(field(obj,key,0.0) for key in ('HoldToInteractTime','MinHoldToInteractTime','GamepadHoldToInteractTime'))
                action=(vec(field(field(obj,'Transform'),'trans')),field(obj,'UseWithinRadius'),hold)
                if sid in ('ID_H_TO_OPEN_DOOR','ID_H_TO_OPEN_GATE'): doors.setdefault(name,[]).append(action)
                elif 'interactbutton_pf' in short: buttons.setdefault(name,[]).append(action)
            if obj.desc.name=='ReferenceObjectData':
                child=ref(db,field(obj,'Blueprint')).split('/')[-1]
                if 'door' in child and not any(x in child for x in ('frame','rail','sound','capbox')):
                    leaves.setdefault(name,[]).append(vec(field(field(obj,'BlueprintTransform'),'trans')))
                    shapes.setdefault(name,[]).append((ref(db,field(obj,'Blueprint'))+'_mesh',transform(field(obj,'BlueprintTransform'))))
    # Carry interaction locations through nested prefab placements.
    frontier=set(doors); expanded=set(doors)
    while frontier:
        children=frontier;frontier=set()
        for parent,imports in deps.items():
            if parent in expanded or not any(names.get(g) in children for g in imports):continue
            db=read(parent);expanded.add(parent)
            if db.prim.desc.name!='SpatialPrefabBlueprint':continue
            for _,obj in db.instances:
                if obj.desc.name!='ReferenceObjectData' or field(obj,'Excluded',False):continue
                child=ref(db,field(obj,'Blueprint'))
                xf=transform(field(obj,'BlueprintTransform'))
                if child==parent:continue
                for pt,radius,hold in doors.get(child,()):
                    doors.setdefault(parent,[]).append((world(xf,pt),radius,hold))
            if parent in doors:frontier.add(parent)
    rows=set(); placed={}; links=[]
    common={'art','arts','gameplay','gen4_only_arts','gen4_only_art','background','art_gen4','gen_4','gen4_map'}
    modes={'conquestlarge':'TurfWarLarge0','conquestsmall':'TurfWarSmall0','conquestsmall0':'TurfWarSmall0',
           'heist':'Heist0','heist0':'Heist0','bloodmoney':'BloodMoney0','hostage':'Hostage0','rescue':'Hostage0',
           'deathmatch':'TeamDeathMatch0','hotwire':'Hotwire0','hit':'Hit0','ctf':'CaptureTheFlag0',
           'squadheist':'SquadHeist0','bountyhunter':'CashGrab0','cashgrab':'CashGrab0'}
    for name in objects:
        prefix=('/levels/mp/' if '/levels/mp/' in name else '/levels/')+path.stem.lower()+'/'
        if prefix not in name:continue
        tail=name.split(prefix,1)[1].split('/')
        scope=modes.get(tail[0],'')
        prefix_tag='@'+scope+' ' if scope else ''
        if (not scope and tail[0] not in common) or len(tail)>2 or 'schematic' in tail[-1] and len(tail)>1:continue
        groupres=name+'/staticmodelgroup_physics_dedicatedserver'
        relevant=any(names.get(g) in doors or names.get(g) in buttons or names.get(g) in leaves for g in deps[name])
        bars=groupres in resources and any('/metalbarsvault_' in names.get(g,'') for g in deps[name])
        if len(tail)>1 and not (relevant or bars):continue
        db=read(name)
        if bars:rows.update(prefix_tag+r for r in collision_spans(db,decompress(resources[groupres]),ref))
        for idx,(gid,obj) in enumerate(db.instances):
            if obj.desc.name!='ReferenceObjectData' or field(obj,'Excluded',False):continue
            prefab=ref(db,field(obj,'Blueprint'))
            if prefab not in doors and prefab not in buttons and prefab not in leaves:continue
            xf=transform(field(obj,'BlueprintTransform'))
            key=(db.fileGUID.format(),gid.format())
            placed[key]=(prefab,xf,scope)
            for local,radius,hold in doors.get(prefab,[]):
                p=world(xf,local)
                kind='double' if 'double' in prefab.split('/')[-1] else 'direct'
                parts=leaves.get(prefab,[])
                gate=p
                if kind=='direct' and len(parts)==1 and 0.65<math.dist(parts[0][::2],local[::2])<1.8:
                    hinge=world(xf,parts[0]);gate=((hinge[0]+p[0])/2,p[1],(hinge[2]+p[2])/2)
                rows.add(prefix_tag+kind+' '+numbers((*gate,*p,radius,hold,*normal(xf,parts,local))))
        def endpoint(n):
            if n & 0x80000000:return tuple(g.format() for g in db.externalGUIDs[n&0x7fffffff])
            return (db.fileGUID.format(),db.instances[n-1][0].format()) if 0<n<=len(db.instances) else None
        arr=field(db.prim,'EventConnections')
        for f in arr.fields if arr else []:
            event=f.value
            if field(field(event,'SourceEvent'),'Id')==-1447021378 and field(field(event,'TargetEvent'),'Id') in (-2137689625,-815149591):
                links.append((endpoint(field(event,'Source')),endpoint(field(event,'Target')),scope))
    wanted={shape for _,dst,_ in links if dst in placed for shape,_ in shapes.get(placed[dst][0],[])}
    bounds=mesh_bounds(store,path.stem,wanted)
    for src,dst,event_scope in links:
        if src not in placed or dst not in placed:continue
        button,bx,bs=placed[src];door,dx,ds=placed[dst];parts=leaves.get(door,[])
        scopes={s for s in (bs,ds,event_scope) if s}
        if len(scopes)>1:continue
        prefix_tag='@'+next(iter(scopes))+' ' if scopes else ''
        if not parts:continue
        axis=normal(dx,parts,None)
        if len(parts)==1:
            shape,matrix=shapes[door][0]
            if shape not in bounds:continue
            box=bounds[shape]
            mid=[(box[i]+box[i+4])*0.5 for i in range(3)]
            center=world(dx,world(matrix,mid))
            wide=0 if box[4]-box[0]>box[6]-box[2] else 2
            tip=mid.copy();tip[wide]+=1
            right=world(dx,world(matrix,tip))
            vx,vz=right[0]-center[0],right[2]-center[2]
            length=math.hypot(vx,vz)
            if length<0.1:continue
            axis=(-vz/length,vx/length)
        else:
            center=world(dx,tuple(sum(p[i] for p in parts)/len(parts) for i in range(3)))
        for local,radius,hold in buttons.get(button,[]):
            control=world(bx,local)
            # Roller-door pivots sit above the passage. The switch indicates the approach floor.
            rows.add(prefix_tag+'button '+numbers((center[0],control[1],center[2],*control,radius,hold,*axis)))
    return sorted(rows)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('server',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--reader',required=True,type=Path);parser.add_argument('--map');parser.add_argument('--client',type=Path,help='Hardline client Data folder for offset roller-door mesh bounds')
    args=parser.parse_args()
    sys.path.insert(0,str(args.reader.resolve()))
    import ebx
    paths=sorted(p for p in args.server.rglob('*.toc') if p.parent.name.lower() in ('mp',p.stem.lower()) and
                 re.fullmatch(r'(?:mp|xp\d+)_[a-z0-9_]+',p.stem,re.I) and (not args.map or p.stem==args.map))
    if not paths:parser.error('no matching dedicated-server multiplayer level found')
    args.output.mkdir(parents=True,exist_ok=True)
    store=Store(args.client) if args.client is not None else None
    with TemporaryDirectory() as work:
        for path in paths:
            rows=scan(path,Path(work,'entry.ebx'),ebx,store)
            dest=args.output/(path.stem.lower()+'.doors')
            scratch=dest.with_suffix('.tmp')
            scratch.write_text('\n'.join(['# Hardline door index v1',*rows])+'\n',encoding='ascii')
            scratch.replace(dest)
            print(path.stem,len(rows),flush=True)
if __name__=='__main__':main()
