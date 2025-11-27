#by Durik256
from inc_noesis import *

def registerNoesisTypes():
    handle = noesis.register("Ultimate Spider Man (PC)", ".pcpack")
    noesis.setHandlerExtractArc(handle, Extract)
    return 1

def Extract(fileName, fileLen, justChecking):
    if justChecking: #it's valid
        return 1

    data = rapi.loadIntoByteArray(fileName)
    bs = NoeBitStream(data)
    
    dummy1 = bs.readUInt()
    dummy2 = bs.readUInt()
    dummy3 = bs.readUInt()
    dummy4 = bs.readUInt()
    dummy5 = bs.readUInt()
    dummy6 = bs.readUInt()
    header_size = bs.readUInt()
    data_off = bs.readUInt()

    offset_list = []
    start = 0
    for _ in range(2):
        start = data.find(b'\xe3\xe3\xe3\xe3', start)
        if start == -1:
            return 0
        start += 4
        bs.seek(start)

    ext_dict = {b'PCM ':'.pcm', b'DDS ':'.dds'}
    counter = 0
    while True:
        some_hash = bs.readUInt()  # ???
        type_val = bs.readUInt()
        if type_val >= 0x1000 or type_val == 0x0000:
            break
        offset = bs.readUInt()
        size = bs.readUInt()

        real_offset = offset + data_off
        if size > 0:
            cpos = bs.tell()
            bs.seek(real_offset)
            export_data = bs.read(size)
            bs.seek(cpos)
            
            name = "%d/%08X%s" % (type_val, counter, ext_dict.get(export_data[:4], '.dat'))
            counter += 1
            rapi.exportArchiveFile(name,  export_data)
            print(name)

    print("Complete!")
    return 1