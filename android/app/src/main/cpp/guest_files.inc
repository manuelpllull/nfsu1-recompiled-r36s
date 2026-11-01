std::string guestWorkingDirectory="C:\\nfsu1";
void logicalDrives(){ret(4,0);}
void driveType(){auto path=lower(guestString(arg(0)));ret(path.empty()||path.rfind("c:",0)==0?3:1,1);}
uint32_t fileError(){switch(errno){case ENOENT:return 2;case EACCES:case EPERM:return 5;case EEXIST:return 80;case ENOSPC:return 112;case EBADF:return 6;default:return 1;}}
// Guest paths resolved case-insensitively, keyed by the lower-case path inside the game folder. The game opens its
// stream archive (STREAML4RA.BUN) many times a second while driving; each resolution listed every directory on the
// way and canonicalised the result (~2% of the game thread in the VM, more on an SD card). Only paths whose every
// component exists are kept, so creating files cannot make an entry wrong; deleting, removing or moving clears it.
std::mutex resolvedGamePathsMutex;
std::unordered_map<std::string,std::filesystem::path> resolvedGamePaths;
void forgetResolvedGamePaths(){std::lock_guard<std::mutex> lock(resolvedGamePathsMutex);resolvedGamePaths.clear();}
std::filesystem::path gamePath(std::string guest){
    std::replace(guest.begin(),guest.end(),'\\','/');auto normalized=lower(guest);
    if(normalized=="c:/nfsu1")guest.clear();
    else if(normalized.rfind("c:/nfsu1/",0)==0)guest=guest.substr(9);
    else if(guest.find(':')!=std::string::npos||guest.rfind("//",0)==0)throw GuestStop("Unsupported guest path: "+guest);
    else {if(!guest.empty()&&guest.front()=='/')guest.erase(0,1);else{auto current=guestWorkingDirectory.substr(8);std::replace(current.begin(),current.end(),'\\','/');if(!current.empty())guest=current+"/"+guest;}}
    auto relative=std::filesystem::path(guest).lexically_normal();for(const auto& part:relative)if(part=="..")throw GuestStop("Guest path leaves nfsu1: "+guest);
    const std::string cacheKey=lower(relative.generic_string());
    {std::lock_guard<std::mutex> lock(resolvedGamePathsMutex);auto cached=resolvedGamePaths.find(cacheKey);if(cached!=resolvedGamePaths.end())return cached->second;}
    auto result=std::filesystem::path(hostGameRoot);bool everyPartExists=true;
    for(const auto& part:relative){if(part==".")continue;std::error_code error;bool found=false;
        for(auto& entry:std::filesystem::directory_iterator(result,error))if(lower(entry.path().filename().string())==lower(part.string())){result=entry.path();found=true;break;}
        if(!found){result/=part;everyPartExists=false;}
    }
    auto root=std::filesystem::weakly_canonical(hostGameRoot);auto resolved=std::filesystem::weakly_canonical(result);
    auto check=resolved.lexically_relative(root);for(const auto& part:check)if(part=="..")throw GuestStop("Guest symlink leaves data root");
    if(everyPartExists){std::lock_guard<std::mutex> lock(resolvedGamePathsMutex);resolvedGamePaths.emplace(cacheKey,resolved);}
    return resolved;
}
std::map<uint32_t,int> fileHandles;uint32_t nextFileHandle=0xf0060000;
// The game skips its CD-drive check when the game folder holds a file named
// "foobar" (its developer switch, 0x0057CAC0("foobar")). Copies made without
// it showed the insert-disc MessageBoxA. Present it as an empty file when absent.
bool discCheckSwitch(const std::filesystem::path& path){
    std::error_code error;
    return lower(path.filename().string())=="foobar"&&!std::filesystem::exists(path,error)&&
        std::filesystem::weakly_canonical(path.parent_path())==std::filesystem::weakly_canonical(hostGameRoot);
}
bool closeGuestFile(uint32_t handle){auto file=fileHandles.find(handle);if(file==fileHandles.end())return false;::close(file->second);fileHandles.erase(file);return true;}
void fileType(){
    uint32_t handle=arg(0);
    if(handle>=0xf0000200u&&handle<=0xf0000208u){ret(2,1);return;}
    if(fileHandles.find(handle)!=fileHandles.end()){ret(1,1);return;}
    lastError=6;ret(0,1);
}
void longPath(){
    auto path=gamePath(guestString(arg(0)));std::error_code error;if(!std::filesystem::exists(path,error)){lastError=2;ret(0,3);return;}
    auto relative=path.lexically_relative(std::filesystem::weakly_canonical(hostGameRoot)).string();
    std::string result="C:\\nfsu1";if(relative!="."&&!relative.empty())result+="\\"+relative;std::replace(result.begin(),result.end(),'/','\\');
    if(arg(2)<=result.size()){ret(result.size()+1,3);return;}std::memcpy(ptr(arg(1),result.size()+1),result.c_str(),result.size()+1);ret(result.size(),3);
}
// Fan-translated font atlases (some repacks): Languages/LanguageTextures.bin holds a translation's font
// atlas whose glyph layout only matches the metrics in that translation's language file (in the repacks
// seen, a Russian one shipped as English.bin: two of its six font tables (chunk 0x30201) differ from every
// other language file). Every other language then draws its small fonts (loading tips, race HUD) from
// rectangles of the original layout: shredded glyphs, also on a PC with those files. When a language
// file's font tables differ from English.bin's and English.bin's strings are mostly 8-bit (a translation),
// the file is served with English.bin's font tables, and its accented Latin-1 letters mapped to plain
// ASCII (the translated atlas has its own letters at those codes: "versión" came out as "versiуn").
// NFSU2_FONT_FIX=0 (NFSU_FONT_FIX=0 in nfsu.cfg) serves the files unchanged.
int languageFileOverlay(const std::filesystem::path& path){
    static const bool enabled=[]{const char* e=std::getenv("NFSU2_FONT_FIX");return !e||std::atoi(e)!=0;}();
    if(!enabled)return -1;
    const std::string name=lower(path.filename().string()),dir=lower(path.parent_path().filename().string());
    if(dir!="languages"||name.size()<5||name.compare(name.size()-4,4,".bin")||name=="english.bin"||name=="languagetextures.bin")return -1;
    auto slurp=[](const std::filesystem::path& p,std::vector<uint8_t>& out){
        int fd=::open(p.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)return false;struct stat st{};if(fstat(fd,&st)||st.st_size<=0||st.st_size>(64<<20)){::close(fd);return false;}
        out.resize(size_t(st.st_size));size_t got=0;while(got<out.size()){ssize_t n=::read(fd,out.data()+got,out.size()-got);if(n<=0)break;got+=size_t(n);}
        ::close(fd);return got==out.size();};
    std::filesystem::path english;std::error_code error;
    for(auto& entry:std::filesystem::directory_iterator(path.parent_path(),error))if(lower(entry.path().filename().string())=="english.bin")english=entry.path();
    std::vector<uint8_t> target,ref;
    if(english.empty()||!slurp(path,target)||!slurp(english,ref))return -1;
    struct Chunk{uint32_t id;size_t at,size;};
    auto chunks=[](const std::vector<uint8_t>& d){std::vector<Chunk> c;size_t i=0;
        while(i+8<=d.size()){uint32_t id,sz;std::memcpy(&id,&d[i],4);std::memcpy(&sz,&d[i+4],4);if(i+8+uint64_t(sz)>d.size())return std::vector<Chunk>{};c.push_back({id,i+8,sz});i+=8+size_t(sz);}
        return c;};
    auto ct=chunks(target),cr=chunks(ref);
    std::vector<const Chunk*> ft,fr;for(auto& c:ct)if(c.id==0x30201)ft.push_back(&c);for(auto& c:cr)if(c.id==0x30201)fr.push_back(&c);
    if(ft.empty()||ft.size()!=fr.size())return -1;
    std::vector<bool> differs(ft.size());bool any=false;
    for(size_t k=0;k<ft.size();++k){differs[k]=ft[k]->size!=fr[k]->size||std::memcmp(&target[ft[k]->at],&ref[fr[k]->at],ft[k]->size);any=any||differs[k];}
    if(!any)return -1;   // consistent install (original fonts everywhere): nothing to do
    // English.bin must be the translation: its strings mostly 8-bit.
    auto stringsOf=[](const std::vector<uint8_t>& d,const Chunk& c,size_t& from,size_t& to){
        if(c.id!=0x39000||c.size<16)return false;uint32_t h[4];std::memcpy(h,&d[c.at],16);
        if(h[3]>=c.size||uint64_t(h[2])+uint64_t(h[1])*8!=h[3])return false;from=c.at+h[3];to=c.at+c.size;return true;};
    size_t rf=0,rt=0;if(cr.empty()||!stringsOf(ref,cr[0],rf,rt))return -1;
    size_t high=0,letters=0;for(size_t i=rf;i<rt;++i){if(ref[i]>=0xc0)++high;else if((ref[i]|0x20)>='a'&&(ref[i]|0x20)<='z')++letters;}
    if(high<letters/2)return -1;   // not a translation into another alphabet: leave it alone
    std::vector<uint8_t> out;out.reserve(target.size()+64);size_t fontIndex=0,remapped=0;
    for(const Chunk& c:ct){
        const uint8_t* src=&target[c.at];size_t size=c.size;
        if(c.id==0x30201){const size_t k=fontIndex++;if(differs[k]){src=&ref[fr[k]->at];size=fr[k]->size;}}
        uint32_t hdr[2]={c.id,uint32_t(size)};const size_t start=out.size();
        out.insert(out.end(),reinterpret_cast<uint8_t*>(hdr),reinterpret_cast<uint8_t*>(hdr)+8);out.insert(out.end(),src,src+size);
        size_t sf=0,st=0;
        if(c.id==0x39000&&stringsOf(target,c,sf,st)){
            static const char latin1[]="AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
            for(size_t i=sf;i<st;++i){uint8_t& b=out[start+8+(i-c.at)];
                if(b>=0xc0){b=uint8_t(latin1[b-0xc0]);++remapped;}else if(b==0xa1){b='!';++remapped;}else if(b==0xbf){b='?';++remapped;}else if(b==0xa9){b='c';++remapped;}}
        }
    }
    int fd=memfd_create("nfsu1-language",MFD_CLOEXEC);if(fd<0)return -1;
    size_t put=0;while(put<out.size()){ssize_t n=::write(fd,out.data()+put,out.size()-put);if(n<=0){::close(fd);return -1;}put+=size_t(n);}
    ::lseek(fd,0,SEEK_SET);
    unsigned replaced=0;for(bool d:differs)replaced+=d;
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Language file %s: font atlas is a translation's (English.bin); %u font tables taken from English.bin, %zu accented letters as ASCII (NFSU_FONT_FIX=0 disables)",
        path.filename().string().c_str(),replaced,remapped);
    return fd;
}
void createFile(){
    auto path=gamePath(guestString(arg(0)));uint32_t access=arg(1),disposition=arg(4),flags=arg(5);
    if(flags&0x40000000u)throw GuestStop("Overlapped CreateFile requires asynchronous file service");
    int options=access&0x40000000u?(access&0x80000000u?O_RDWR:O_WRONLY):O_RDONLY;
    switch(disposition){case 1:options|=O_CREAT|O_EXCL;break;case 2:options|=O_CREAT|O_TRUNC;break;case 3:break;case 4:options|=O_CREAT;break;case 5:options|=O_TRUNC;break;default:lastError=87;ret(UINT32_MAX,7);return;}
    bool existed=std::filesystem::exists(path);
    int descriptor=disposition==3&&!(access&0x40000000u)&&discCheckSwitch(path) ? ::open("/dev/null",O_RDONLY|O_CLOEXEC) : -1;
    if(descriptor<0&&disposition==3&&!(access&0x40000000u))descriptor=languageFileOverlay(path);
    if(descriptor<0)descriptor=::open(path.c_str(),options|O_CLOEXEC,0600);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest file %s access=%x disposition=%u",path.filename().string().c_str(),access,disposition);
    if(descriptor<0){lastError=fileError();ret(UINT32_MAX,7);return;}uint32_t handle=nextFileHandle++;fileHandles.emplace(handle,descriptor);lastError=(disposition==2||disposition==4)&&existed?183:0;ret(handle,7);
}
void readFile(){
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,5);return;}if(arg(4))throw GuestStop("Overlapped ReadFile pending");
    if(arg(3))write32(arg(3),0);ssize_t count;
    const auto readStart=std::chrono::steady_clock::now();
    do{count=::read(file->second,ptr(arg(1),arg(2)),arg(2));}while(count<0&&errno==EINTR);
    if(nfsIsGameThread()){nfsWaitAdd(10,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-readStart).count());if(count>0)nfsWaitAdd(11,uint64_t(count));}
    if(count<0){lastError=fileError();ret(0,5);return;}if(arg(3))write32(arg(3),count);ret(1,5);
}
void writeFile(){
    if(arg(4))throw GuestStop("Overlapped WriteFile pending");
    if(!arg(3)||!memory->writable(arg(3),4)){lastError=87;ret(0,5);return;}write32(arg(3),0);
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,5);return;}
    if(!memory->readable(arg(1),arg(2))){lastError=1784;ret(0,5);return;}
    ssize_t count;do{count=::write(file->second,ptr(arg(1),arg(2)),arg(2));}while(count<0&&errno==EINTR);
    if(count<0){lastError=fileError();ret(0,5);return;}write32(arg(3),uint32_t(count));ret(1,5);
}
void flushFileBuffers(){auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,1);return;}
    int result;do{result=::fsync(file->second);}while(result&&errno==EINTR);if(result){lastError=fileError();ret(0,1);return;}ret(1,1);
}
void setEndOfFile(){auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,1);return;}
    auto position=::lseek(file->second,0,SEEK_CUR);if(position<0||::ftruncate(file->second,position)){lastError=fileError();ret(0,1);return;}ret(1,1);
}
void fileSize(){auto file=fileHandles.find(arg(0));struct stat status{};if(file==fileHandles.end()){lastError=6;ret(UINT32_MAX,2);return;}if(fstat(file->second,&status)){lastError=fileError();ret(UINT32_MAX,2);return;}uint64_t size=status.st_size;if(arg(1))write32(arg(1),size>>32);lastError=0;ret(size,2);}
void fileAttributes(){auto path=gamePath(guestString(arg(0)));struct stat status{};if(discCheckSwitch(path)){ret(0x20,1);return;}if(stat(path.c_str(),&status)){lastError=fileError();ret(UINT32_MAX,1);return;}ret(S_ISDIR(status.st_mode)?0x10:0x20,1);}
void setFilePointer(){
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(UINT32_MAX,4);return;}
    uint32_t method=arg(3),high=arg(2);if(method>2){lastError=87;ret(UINT32_MAX,4);return;}
    int64_t distance=int32_t(arg(1));if(high){uint64_t bits=(uint64_t(read32(high))<<32)|arg(1);std::memcpy(&distance,&bits,8);}
    off_t result=::lseek(file->second,off_t(distance),method==0?SEEK_SET:method==1?SEEK_CUR:SEEK_END);
    if(result<0){lastError=errno==EINVAL?131:fileError();ret(UINT32_MAX,4);return;}
    if(!high&&uint64_t(result)>UINT32_MAX){lastError=87;ret(UINT32_MAX,4);return;}
    if(high)write32(high,uint64_t(result)>>32);lastError=0;ret(uint32_t(result),4);
}
// Keep Windows profile folders alongside the user's game data on Android.
void shellFolderPath(){
    uint32_t folder=arg(1),out=arg(4);if(!out||!memory->writable(out,260)){ret(0x80070057u,5);return;}
    *static_cast<char*>(ptr(out,1))=0;
    if(arg(2)||arg(3)>1||(folder&~0xc0ffu)){ret(0x80070057u,5);return;}
    std::string suffix;switch(folder&0xff){case 0x1c:suffix="AppData\\Local";break;case 0x1a:suffix="AppData\\Roaming";break;case 0x23:suffix="AppData\\Local";break;case 5:suffix="Documents";break;default:ret(0x80070057u,5);return;}
    std::string logical="C:\\nfsu1\\"+suffix;auto path=gamePath(logical);std::error_code error;
    if(folder&0x8000)std::filesystem::create_directories(path,error);
    if(error){ret(0x80070005u,5);return;}
    if(!(folder&0x4000)&&!std::filesystem::is_directory(path,error)){ret(0x80070003u,5);return;}
    std::memcpy(ptr(out,logical.size()+1),logical.c_str(),logical.size()+1);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest profile folder %s",logical.c_str());ret(0,5);
}
struct GuestFileSearch {std::vector<std::filesystem::path> paths;size_t next=0;};
std::map<uint32_t,GuestFileSearch> fileSearches;uint32_t nextSearchHandle=0xf0070000;
bool filePatternMatches(std::string pattern,std::string name){
    pattern=lower(pattern);name=lower(name);if(pattern=="*.*")pattern="*";
    size_t p=0,n=0,star=std::string::npos,retry=0;
    while(n<name.size()){
        if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==name[n])){++p;++n;}
        else if(p<pattern.size()&&pattern[p]=='*'){star=p++;retry=n;}
        else if(star!=std::string::npos){p=star+1;n=++retry;}else return false;
    }while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();
}
bool writeFindData(uint32_t out,const std::filesystem::path& path){
    if(!out||!memory->writable(out,320)){lastError=87;return false;}
    struct stat status{};if(::stat(path.c_str(),&status)){lastError=fileError();return false;}
    auto name=path.filename().string();if(name.size()>=260){lastError=206;return false;}
    std::memset(ptr(out,320),0,320);write32(out,S_ISDIR(status.st_mode)?0x10:0x20);
    auto time=[&](uint32_t offset,int64_t seconds){uint64_t ticks=(seconds+11644473600ll)*10000000ull;std::memcpy(ptr(out+offset,8),&ticks,8);};
    time(4,status.st_ctime);time(12,status.st_atime);time(20,status.st_mtime);
    uint64_t size=S_ISDIR(status.st_mode)?0:uint64_t(status.st_size);write32(out+28,size>>32);write32(out+32,uint32_t(size));
    std::memcpy(ptr(out+44,name.size()+1),name.c_str(),name.size()+1);return true;
}
void findFirstFile(){
    auto requested=guestString(arg(0));std::replace(requested.begin(),requested.end(),'\\','/');
    if(requested.empty()||requested.back()=='/'){lastError=123;ret(UINT32_MAX,2);return;}
    auto separator=requested.find_last_of('/');auto pattern=requested.substr(separator==std::string::npos?0:separator+1);
    auto directory=gamePath(separator==std::string::npos?".":requested.substr(0,separator));
    std::error_code error;GuestFileSearch search;
    if(std::filesystem::is_directory(directory,error))for(const char* dot:{".",".."})if(filePatternMatches(pattern,dot))search.paths.push_back(directory/dot);
    for(auto& entry:std::filesystem::directory_iterator(directory,error)){
        if(!filePatternMatches(pattern,entry.path().filename().string()))continue;
        // Apply the same containment check used for ordinary file opens.
        auto logical="C:/nfsu1/"+entry.path().lexically_relative(std::filesystem::weakly_canonical(hostGameRoot)).string();
        gamePath(logical);search.paths.push_back(entry.path());
    }
    if(error){lastError=error==std::errc::no_such_file_or_directory?3:5;ret(UINT32_MAX,2);return;}
    if(search.paths.empty()){lastError=2;ret(UINT32_MAX,2);return;}
    std::sort(search.paths.begin(),search.paths.end());
    if(!writeFindData(arg(1),search.paths.front())){ret(UINT32_MAX,2);return;}
    search.next=1;uint32_t handle=nextSearchHandle++;fileSearches.emplace(handle,std::move(search));ret(handle,2);
}
void findNextFile(){auto found=fileSearches.find(arg(0));if(found==fileSearches.end()){lastError=6;ret(0,2);return;}
    auto& search=found->second;if(search.next==search.paths.size()){lastError=18;ret(0,2);return;}
    if(!writeFindData(arg(1),search.paths[search.next])){ret(0,2);return;}++search.next;ret(1,2);
}
void findClose(){if(!fileSearches.erase(arg(0))){lastError=6;ret(0,1);return;}ret(1,1);}
void createDirectory(){if(arg(1))throw GuestStop("CreateDirectory security attributes pending");auto path=gamePath(guestString(arg(0)));
    if(::mkdir(path.c_str(),0700)){lastError=errno==EEXIST?183:fileError();ret(0,2);return;}ret(1,2);
}
void fullPathName(){
    auto requested=guestString(arg(0));if(requested.empty()){lastError=123;ret(0,4);return;}
    auto path=gamePath(requested);auto relative=path.lexically_relative(std::filesystem::weakly_canonical(hostGameRoot)).string();
    std::string logical="C:\\nfsu1";if(relative!="."&&!relative.empty())logical+="\\"+relative;
    std::replace(logical.begin(),logical.end(),'/','\\');if(requested.back()=='/'||requested.back()=='\\')logical+='\\';
    uint32_t capacity=arg(1),out=arg(2),part=arg(3);if(capacity<=logical.size()){ret(uint32_t(logical.size()+1),4);return;}
    if(!out||!memory->writable(out,logical.size()+1)||(part&&!memory->writable(part,4))){lastError=87;ret(0,4);return;}
    std::memcpy(ptr(out,logical.size()+1),logical.c_str(),logical.size()+1);
    if(part)write32(part,logical.back()=='\\'?0:out+uint32_t(logical.find_last_of('\\')+1));ret(uint32_t(logical.size()),4);
}
// DeleteFileA / MoveFileA inside the game folder (gamePath refuses anything outside it).
void deleteFile(){
    auto path=gamePath(guestString(arg(0)));
    if(::unlink(path.c_str())){lastError=fileError();ret(0,1);return;}
    forgetResolvedGamePaths();lastError=0;ret(1,1);
}
void removeDirectory(){
    auto path=gamePath(guestString(arg(0)));
    if(::rmdir(path.c_str())){lastError=errno==ENOTEMPTY||errno==EEXIST?145:fileError();ret(0,1);return;}  // ERROR_DIR_NOT_EMPTY
    forgetResolvedGamePaths();lastError=0;ret(1,1);
}
void moveFile(){
    auto from=gamePath(guestString(arg(0))),to=gamePath(guestString(arg(1)));
    struct stat status{};if(!::stat(to.c_str(),&status)){lastError=183;ret(0,2);return;}  // ERROR_ALREADY_EXISTS
    if(::rename(from.c_str(),to.c_str())){lastError=fileError();ret(0,2);return;}
    forgetResolvedGamePaths();lastError=0;ret(1,2);
}
// GetFileTime(handle, creation, access, write): FILETIME is 100 ns ticks since 1601.
void fileTime(){
    auto file=fileHandles.find(arg(0));struct stat status{};
    if(file==fileHandles.end()){lastError=6;ret(0,4);return;}
    if(fstat(file->second,&status)){lastError=fileError();ret(0,4);return;}
    auto ticks=[](const timespec& time){return uint64_t(time.tv_sec)*10000000ull+uint64_t(time.tv_nsec)/100u+116444736000000000ull;};
    uint64_t values[3]={ticks(status.st_mtim),ticks(status.st_atim),ticks(status.st_mtim)};  // no creation time: use the last write
    for(unsigned i=0;i<3;++i)if(arg(1+i))std::memcpy(ptr(arg(1+i),8),&values[i],8);
    lastError=0;ret(1,4);
}
const std::map<std::string,recomp_func_t> fileFunctions={
    {"GetFileTime",fileTime},{"DeleteFileA",deleteFile},{"RemoveDirectoryA",removeDirectory},{"MoveFileA",moveFile},
    {"GetFullPathNameA",fullPathName},{"WriteFile",writeFile},{"FlushFileBuffers",flushFileBuffers},{"SetEndOfFile",setEndOfFile},
    {"FindFirstFileA",findFirstFile},{"FindNextFileA",findNextFile},{"FindClose",findClose},{"CreateDirectoryA",createDirectory},
    {"SetFilePointer",setFilePointer},
    {"GetLogicalDrives",logicalDrives},{"GetDriveTypeA",driveType},
    {"GetLongPathNameA",longPath},{"CreateFileA",createFile},{"ReadFile",readFile},{"GetFileSize",fileSize},{"GetFileAttributesA",fileAttributes}
};
