/* 
 * Copyright (C) 2013-2014  LINK/2012 <dma_2012@hotmail.com>
 * Licensed under GNU GPL v3, see LICENSE at top level directory.
 * 
 *
 */
#include "streaming.hpp"
#include "CdStreamInfo.h"
#include <modloader/util/injector.hpp>
#include <modloader/util/path.hpp>
using namespace modloader;

/*
    uiUnk2 flag
        0x02 - cannot delete
        0x04 - streaming is not owner (?)
        0x08 - dependency
        0x10 - first priority request
        0x20 - don't delete on makespacefor

        notes:
        (0x20|0x4) -> gives model alpha=255 otherwise alpha=0
*/

// TODO needs to refresh ifp too
// TODO remove is hoodlum
// TODO avoid CdStream optimization of uiUnknown1 / lastposn / etc (?)
// TODO fix non find (only add) on LoadCdDirectory (GAME FOR COL/IFP/RRR/ETC)
// TODO special model
// TODO clothes
// TODO something is wrong with installed models (runtime) streaming size catching
// TODO proper logging
// TODO proper IsClothes (player_parachute.scm isn't a clothing item)

CAbstractStreaming streaming;

// Assembly hooks at "asm/" folder
extern "C"
{
    CStreamingInfo* ms_aInfoForModel = memory_pointer(0x8E4CC0).get(); // TODO fastman
    extern void HOOK_RegisterNextModelRead();
    extern void HOOK_NewFile();
    extern void HOOK_SetStreamName();
    extern void HOOK_SetImgDscName();
};







/*
 *  Constructs the abstract streaming object 
 */
CAbstractStreaming::CAbstractStreaming()
{
    DWORD SectorsPerCluster, BytesPerSector, NumberOfFreeClusters, TotalNumberOfClusters;

    InitializeCriticalSection(&cs);
    
    /*
     *  Find out if the number of bytes per sector from this disk is 2048 aligned, this allows no-buffering for 2048 bytes aligned files.
     *
     *  FIXME: The game might not work if it is installed in another disk other than the main disk because of the NULL parameter
     *  in GetDiskFreeSpace(). R* did this mistake and I'm doing it again because I'm too lazy to fix R* mistake.
     * 
     */
    if(GetDiskFreeSpaceA(0, &SectorsPerCluster, &BytesPerSector, &NumberOfFreeClusters, &TotalNumberOfClusters)
    && BytesPerSector)
    {
        this->bIs2048AlignedSector  = (2048 % BytesPerSector) == 0;
    }
    else this->bIs2048AlignedSector = false;
}

CAbstractStreaming::~CAbstractStreaming()
{
    DeleteCriticalSection(&cs);
}


/*
 *  CAbstractStreaming::InfoForModel
 *      Returns the streaming info pointer for the specified resource id
 */
CStreamingInfo* CAbstractStreaming::InfoForModel(id_t id)
{
    return &ms_aInfoForModel[id];
}

/*
 *  CAbstractStreaming::IsModelOnStreaming
 *      Checks if the specified model is on the streaming, either on the bus to get loaded or already loaded.
 */
bool CAbstractStreaming::IsModelOnStreaming(id_t id)
{
    return InfoForModel(id)->uiLoadFlag != 0;
}

/*
 *  CAbstractStreaming::RequestModel
 *      Requests a resource into the streaming, essentially loading it
 *      Notice the model will not be available right after the call, it's necessary to wait for it.
 *      Flags are unknown... still researching about them.
 */
void CAbstractStreaming::RequestModel(id_t id, uint32_t flags)
{
    injector::cstd<void(int, int)>::call(0x4087E0, id, flags);
}

/*
 *  CAbstractStreaming::RemoveModel
 *      Removes a resource from the streaming, essentially unloading it
 *      Flags are unknown (and optional) so leave it as default.
 */
void CAbstractStreaming::RemoveModel(id_t id)
{
    injector::cstd<void(int)>::call(0x4089A0, id);
}

/*
 *  CAbstractStreaming::ReloadModel
 *      Reloads the specified resource on the streaming
 *      Notice the model will not be available right after the call, it's necessary to wait for it.
 */
void CAbstractStreaming::ReloadModel(id_t id)
{
    this->RemoveModel(id);
    this->RequestModel(id, InfoForModel(id)->uiUnknown2_ld);
}

/*
 *  CAbstractStreaming::LoadAllRequestedModels
 *      Free ups the streaming bus by loading everything previosly requested
 */
void CAbstractStreaming::LoadAllRequestedModels()
{
    injector::cstd<void()>::call(0x5619D0);         // CTimer::StartUserPause
    injector::cstd<void(int)>::call(0x40EA10, 0);   // CStreaming::LoadAllRequestedModels
    injector::cstd<void()>::call(0x561A00);         // CTimer::EndUserPause
}





/*
 *  CAbstractStreaming::RegisterModelIndex
 *      Registers the existence of an index assigned to a specific filename
 */
void CAbstractStreaming::RegisterModelIndex(const char* filename, id_t index)
{
    if(this->indices.emplace(modloader::hash(filename, ::tolower), index).second == false)
        plugin_ptr->Log("Warning: Model %s appears more than once in abstract streaming", filename);
}

/*
 *  CAbstractStreaming::RegisterStockEntry
 *      Registers the stock/default/original cd directory data of an index, important to restore it later when necessary.
 */
void CAbstractStreaming::RegisterStockEntry(const char* filename, CDirectoryEntry& entry, id_t index, int img_id)
{
    if(this->cd_dir.emplace(std::piecewise_construct,
        std::forward_as_tuple(index),
        std::forward_as_tuple(filename, entry, img_id)).second == false)
    {
        // Please note @entry here is incomplete because the game null terminated the string before the extension '.'
        // So let's use @filename
        plugin_ptr->Log("Warning: Stock entry %s appears more than once in abstract streaming", filename);
    }
}


/*
 *  CAbstractStreaming::InstallFile
 *      Installs a model, clothing or any streamable file, refreshing them after the process
 */
bool CAbstractStreaming::InstallFile(const modloader::file& file)
{
    // If the streaming hasn't initialized we cannot assume much things about the streaming
    // One thing to keep in mind is that in principle stuff should load in alpha order (for streamed scenes etc)
    if(!this->bHasInitializedStreaming)
    {
        // Just push it to this list and it will get loaded when the streaming initializes
        // At this point we don't know if this is a clothing item or an model, for that reason "raw"
        // The initializer will take care of filtering clothes and models from the list
        this->raw_models[file.FileName()] = &file;
        return true;
    }
    else
    {
        // We cannot do much at this point, too many calls may come, repeated calls, uninstalls, well, many things will still happen
        // so we'll delay the actual install to the next frame, put everything on an import list
        this->BeginUpdate();

        if(!IsClothes(file))
        {
            this->mToImportList[file.hash] = &file;
            return true;
        }
        else
        {
            // TODO clothes
        }
    }
    return false;
}

/*
 *  CAbstractStreaming::UninstallFile
 *      Uninstalls a specific file
 */
bool CAbstractStreaming::UninstallFile(const modloader::file& file)
{
    // Ahhh, see the comments at InstallFile.....
    if(!this->bHasInitializedStreaming)
    {
        // Streaming hasn't initialized, just remove it from our raw list
        raw_models.erase(file.FileName());
        return true;
    }
    else
    {
        this->BeginUpdate();

        if(!IsClothes(file))
        {
            // Mark the specified file [hash] to be vanished
            this->mToImportList[file.hash] = nullptr;
            return true;
        }
        else
        {
            // TODO clothes
        }
    }
    return false;
}


/*
 *  CAbstractStreaming::ReinstallFile
 *      Does the same as InstallFile
 */
bool CAbstractStreaming::ReinstallFile(const modloader::file& file)
{
    // Reinstalling works the same way as installing
    return InstallFile(file);
}


void CAbstractStreaming::Update()
{
    if(this->IsUpdating())
    {
        this->ProcessRefreshes();
        this->EndUpdate();
    }
}




void CAbstractStreaming::ImportModels(ref_list<const modloader::file*> files)
{
    LoadCustomCdDirectory(files);
}

void CAbstractStreaming::UnimportModel(id_t index)
{
    auto cd_it    = cd_dir.find(index);         // TODO CHECK FOR FAILURE (IF FAIL NOT ORIGINAL)

    auto& cd = cd_it->second;

    // TODO PUSH THIS TO A FUNC
    auto& model = *InfoForModel(index);
    model.iBlockOffset = cd.offset;
    model.iBlockCount = cd.blocks;
    model.uiUnknown1 = -1;                    // TODO find the one pointing to me and do -1 on it
    model.uiUnknown2_ld = 0;                  // TODO what to do with this one?? It's set during gameplay
    model.ucImgId = cd.img;

    imports.erase(index);
}


/*
 *  Streaming thread
 *      This thread reads pieces from cd images and in mod loader on disk files
 */
int __stdcall CdStreamThread()
{
    DWORD nBytesReaden;
    
    // Get reference to the addresses we'll use
    CdStreamInfo& cdinfo = *memory_pointer(0x8E3FEC).get<CdStreamInfo>();
    
    // Loop in search of things to load in the queue
    while(true)
    {
        int i = -1;
        CdStream* cd;
        bool bIsAbstract = false;
        CAbstractStreaming::AbctFileHandle* sfile = nullptr;
        
        // Wait until there's something to be loaded...
        WaitForSingleObject(cdinfo.semaphore, -1);
        
        // Take the stream index from the queue
        i = GetFirstInQueue(&cdinfo.queue);
        if(i == -1) continue;
        
        cd = &cdinfo.pStreams[i];
        cd->bInUse = true;          // Mark the stream as under work
        if(cd->status == 0)
        {
            // Setup vars
            size_t bsize  = cd->nSectorsToRead;
            size_t offset = cd->nSectorOffset  << 11;       // translate 2KiB based offset to actual offset
            size_t size   = bsize << 11;                    // translate 2KiB based size to actual size
            HANDLE hFile  = (HANDLE) cd->hFile;
            bool bResult  = false;
            const char* filename = nullptr; int index = -1; // When abstract those fields are valid
            
            // Try to find abstract file from hFile
            if(true)
            {
                scoped_lock xlock(streaming.cs);
                auto it = std::find(streaming.stm_files.begin(), streaming.stm_files.end(), hFile);
                if(it != streaming.stm_files.end())
                {
                    bIsAbstract = true;
                    
                    // Setup vars based on abstract file
                    sfile  = &(*it);
                    offset = 0;
                    size   = (size_t) sfile->info.file->Size();
                    bsize  = GetSizeInBlocks(size);
                    index  = sfile->index;
                    filename = sfile->info.file->FileBuffer();
                }
            }
            
            
            // Setup overlapped structure
            cd->overlapped.Offset     = offset;
            cd->overlapped.OffsetHigh = 0;
            
            // Read the stream
            if(ReadFile(hFile, cd->lpBuffer, size, &nBytesReaden, &cd->overlapped))
            {
                bResult = true;
            }
            else
            {
                if(GetLastError() == ERROR_IO_PENDING)
                {
                    // This happens when the stream was open for async operations, let's wait until everything has been read
                    bResult = GetOverlappedResult(hFile, &cd->overlapped, &nBytesReaden, true) != 0;
                }
            }
            
            // There's some real problem if we can't load a abstract model
            if(bIsAbstract && !bResult)
                plugin_ptr->Log("Warning: Failed to load abstract model file %s; error code: 0x%X", filename, GetLastError());
            

            // Set the cdstream status, 0 for "okay" and 254 for "failed to read"
            cd->status = bResult? 0 : 254;
        }
        
        // Remove from the queue what we just readed
        RemoveFirstInQueue(&cdinfo.queue);
        
        // Cleanup
        if(bIsAbstract) streaming.CloseModel(sfile);
        cd->nSectorsToRead = 0;
        if(cd->bLocked) ReleaseSemaphore(cd->semaphore, 1, 0);
        cd->bInUse = false;
    }
    return 0;
}

/*
 *  CAbstractStreaming::Patch
 *      Patches the default game streaming pipeline to have our awesome hooks.
 */
void CAbstractStreaming::Patch()
{
    typedef function_hooker<0x5B8E1B, void()> sinit_hook;
    typedef function_hooker<0x40CF34, int(int, void*, int, int)> cdread_hook;
    bool isHoodlum = injector::address_manager::singleton().IsHoodlum(); // TODO REMOVE, PUT ON ADDR TRANSLATOR

    // Initialise the streaming
    make_static_hook<sinit_hook>([this](sinit_hook::func_type LoadCdDirectory1)
    {
        TempCdDir_t cd_dir;
        this->FetchCdDirectories(cd_dir, LoadCdDirectory1);         // Fetch...
        this->LoadCdDirectories(cd_dir);                            // ...and load
        this->LoadCustomCdDirectory(refs_mapped(raw_models));       // Load custom

        // Mark streaming as initialized
        this->bHasInitializedStreaming = true;
        this->raw_models.clear();
    });


    if(true)
    {
        // Making our our code for the stream thread would make things so much better
        MakeJMP(0x406560, raw_ptr(CdStreamThread));

        // We need to know the next model to be read before the CdStreamRead call happens
        MakeCALL(0x40CCA6, raw_ptr(HOOK_RegisterNextModelRead));
        MakeNOP(0x40CCA6 + 5, 2);

        // We need to return a new hFile if the file is on disk
        MakeCALL(!isHoodlum? 0x406A5B : 0x0156C2FB, raw_ptr(HOOK_NewFile));
        MakeNOP((!isHoodlum? 0x406A5B : 0x0156C2FB)+5, 1);

        // We need to know the model index that will pass throught CallGetAbstractHandle
        make_static_hook<cdread_hook>([](cdread_hook::func_type CdStreamRead, int& streamNum, void*& buf, int& sectorOffset, int& sectorCount)
        {
            iModelBeingLoaded = iNextModelBeingLoaded;
            auto result = CdStreamRead(streamNum, buf, sectorOffset, sectorCount);
            iModelBeingLoaded = iNextModelBeingLoaded = -1;
            return result;
        });
    }


    /*
     *  We need to hook some game code where imgDescriptor[x].name and SteamNames[x][y] is set because they have a limit in filename size,
     *  and we do not want that limit.
     * 
     *  This is a real dirty solution, but well...
     *  
     *  How does it work?
     *      First of all, we hook the StreamNames string copying to copy our dummy string "?\0"
     *                    this is simple and not dirty, the game never accesses this string again,
     *                    only to check if there's a string there (and if there is that means the stream is open)
     *  
     *      Then comes the dirty trick:
     *          We need to hook the img descriptors (CImgDescriptor array) string copying too,
     *          but we need to do more than that, because this string is still used in CStreaming::LoadCdDirectory
     *          to open the file and read the header.
     * 
     *          So what we did? The first field in CImgDescriptor is a char[40] array to store the name, so, we turned this field into an:
     *              union {
     *                  char name[40];
     *                  
     *                  struct {
     *                      char dummy[2];      // Will container the dummy string "?\0" (0x003F)
     *                      char pad[2];        // Padding to 4 bytes boundary
     *                      char* customName;   // Pointer to a static buffer containing the new file name
     *                  };
     *              };
     * 
     *          Then we hook CStreaming::LoadCdDirectory to give the pointer customName instead of &name to CFileMgr::Open
     *          Very dirty, isn't it?
     */
    if(true)
    {
        // Warning: do not use function_hooker here in this context, it would break many scoped hooks in this plugin.
        static void*(*OpenFile)(const char*, const char*);
        static auto OpenFileHook = [](const char* filename, const char* mode)
        {
            return OpenFile(GetCdStreamPath(filename), mode);
        };


        size_t nopcount = injector::address_manager::singleton().IsSteam()? 0xC : 0xA;

        // Resolve the cd stream filenames by ourselves
        OpenFile = MakeCALL(0x5B6183, raw_ptr((decltype(OpenFile))(OpenFileHook))).get();

        //
        MakeNOP(!isHoodlum? 0x406886 : 0x01564B90, nopcount);
        MakeCALL(!isHoodlum? 0x406886 : 0x01564B90, raw_ptr(HOOK_SetStreamName));
        MakeNOP(!isHoodlum? 0x407642 : 0x01567BC2, nopcount);
        MakeCALL(!isHoodlum? 0x407642 : 0x01567BC2, raw_ptr(HOOK_SetImgDscName));
    }
}
