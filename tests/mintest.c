// mintest.c - 最小 FspFileSystemCreate 复现
#include <windows.h>
#include <winfsp.h>
#include <stdio.h>

int wmain(void) {
    FSP_FSCTL_VOLUME_PARAMS vp = {};
    vp.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    vp.SectorSize = 4096;
    vp.SectorsPerAllocationUnit = 1;
    FSP_FILE_SYSTEM* fs = nullptr;
    FSP_FILE_SYSTEM_INTERFACE itf = {};
    NTSTATUS st = FspFileSystemCreate((PWSTR)L"WinFsp", &vp, &itf, &fs);
    printf("FspFileSystemCreate => 0x%08X LastError=%lu\n", st, GetLastError());
    return 0;
}
