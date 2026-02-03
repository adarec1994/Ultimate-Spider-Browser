#by Durik256
from inc_noesis import *

def registerNoesisTypes():
    handle = noesis.register("Ultimate Spider Man (PC)", ".pcm")
    noesis.setHandlerTypeCheck(handle, noepyCheckType)
    noesis.setHandlerLoadModel(handle, noepyLoadModel)
    return 1

def noepyCheckType(data):
    return 1
	
def noepyLoadModel(data, mdlList):
    bs = NoeBitStream(data)
    ctx = rapi.rpgCreateContext()
    
    bs.seek(8)
    
    num = bs.readUInt()
    ofs = bs.readUInt()
    
    bs.seek(ofs)
    inf_arr = []
    for x in range(num):
        inf_arr.append(bs.read('2H2I'))
        print(inf_arr[-1])
        
    bones = []
        
    for _ in inf_arr:
        if _[1] == 512:
            bs.seek(_[2])
            name_ofs = bs.readUInt()
            zero1 = bs.readUInt()
            num_sm = bs.readUInt()
            inf_sm_ofs = bs.readUInt()
            num_bn = bs.readUInt()
            ofs_bn = bs.readUInt()
            unk = bs.readUInt() # (1) next_mdl?
            ofs_unk = bs.readUInt() # (1) ofs_next_mdl?
            _ = bs.read('4f')
            
            bs.seek(name_ofs+4)
            name = bs.read(28).rstrip(b'\x00').decode('ascii', 'ignore')
            print('mdl_name:', name)
            
            print("name_ofs ", name_ofs)
            print("zero1 ", zero1)
            print("num_sm ", num_sm)
            print("inf_sm_ofs ", inf_sm_ofs)
            print("num_bn ", num_bn)
            print("ofs_bn ", ofs_bn)
            print("unk ", unk)
            print("ofs_unk ", ofs_unk)
            
            
            bs.seek(ofs_bn)
            for x in range(num_bn):
                matrix = NoeMat44.fromBytes(bs.read(64)).toMat43()
                bones.append(NoeBone(x,'bone_%i'%x,matrix,None,x-1))
            
            bs.seek(inf_sm_ofs)
            inf_sm_ofs_arr = []
            for _ in range(num_sm):
                zero = bs.readUInt()
                sm_ofs = bs.readUInt()
                inf_sm_ofs_arr.append(sm_ofs)
                print('    sm_inf_ofs:', zero, sm_ofs)
                
            for _ in inf_sm_ofs_arr:
                bs.seek(_)
                print('Submesh_inf>>>>>>')
                name_ofs = bs.readUInt()
                zero1 = bs.readUInt()
                unk0 = bs.readUInt() # 3?
                unk0_ofs = bs.readUInt()
                _ = bs.read('4f')
                unk_uint = bs.readUInt()
                zero2 = bs.readUInt()
                itype = bs.readUInt() # 5-strip
                inum = bs.readUInt() 
                iofs = bs.readUInt()
                zero3 = bs.readUInt()
                vnum = bs.readUInt()
                vofs = bs.readUInt()
                unks = bs.read('8I')

                bs.seek(name_ofs+4)
                sm_name = bs.read(28).rstrip(b'\x00').decode('ascii', 'ignore')
                print('    sm_name:', sm_name)

                print("    name_ofs ", name_ofs)
                print("    zero1 ", zero1)
                print("    unk0 ", unk0)
                print("    unk0_ofs ", unk0_ofs)
                print("    unk_uint ", unk_uint)
                print("    zero2 ", zero2)
                print("    itype ", itype)
                print("    inum ", inum)
                print("    iofs ", iofs)
                print("    zero3 ", zero3)
                print("    vnum ", vnum)
                print("    vofs ", vofs)
                print("    unks ", unks)
                
                bs.seek(unk0_ofs)
                _ = bs.read('%iH'%unk0)
                print("    _ ::", _ )
                
                
                if not _:
                    _ = None
                rapi.rpgSetBoneMap(_)
                
                stride = unks[2]#64
                
                bs.seek(vofs)
                vbuf = bs.read(vnum*stride)
                
                bs.seek(iofs)
                ibuf = bs.read(inum*2)
                
                try:
                    rapi.rpgSetName(name+"_"+sm_name)
                    rapi.rpgBindPositionBuffer(vbuf, noesis.RPGEODATA_FLOAT, stride)
                    if stride == 64:
                        rapi.rpgBindNormalBufferOfs(vbuf, noesis.RPGEODATA_FLOAT, stride, 12)
                        rapi.rpgBindBoneIndexBufferOfs(vbuf, noesis.RPGEODATA_FLOAT, stride, 32, 4)
                        rapi.rpgBindBoneWeightBufferOfs(vbuf, noesis.RPGEODATA_FLOAT, stride, 48, 4)
                        
                    elif stride == 24:
                        rapi.rpgBindUV1BufferOfs(vbuf, noesis.RPGEODATA_FLOAT, stride, 12)

                    
                    ifmt = noesis.RPGEO_TRIANGLE_STRIP
                    if itype == 4:
                        ifmt = noesis.RPGEO_TRIANGLE
                    
                    rapi.rpgCommitTriangles(ibuf, noesis.RPGEODATA_USHORT, inum, ifmt)
                    rapi.rpgClearBufferBinds()
                except:
                    print('ERROR CREATE MESH SKIP...>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>')
    
    rapi.rpgSetOption(noesis.RPGOPT_TRIWINDBACKWARD, 1)
    try:
        mdl = rapi.rpgConstructModel()
    except:
        mdl = NoeModel()

    mdl.setBones(bones)
    mdl.setModelMaterials(NoeModelMaterials([], [NoeMaterial('default','')]))
    mdlList.append(mdl)
    return 1