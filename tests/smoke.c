/* neutron smoke test: load lsteamclient.dll in Wine and reach the macOS steamclient.dylib. */
#include <windows.h>
#include <stdio.h>

typedef void *(__cdecl *CreateInterfaceFn)(const char *name, int *rc);
typedef int (*CreateSteamPipeFn)(void *self);

int main(void)
{
    HMODULE mod = LoadLibraryA("lsteamclient.dll");
    printf("LoadLibrary(lsteamclient.dll) = %p (err %lu)\n", (void *)mod, mod ? 0 : GetLastError());
    if (!mod) return 1;

    CreateInterfaceFn create = (CreateInterfaceFn)GetProcAddress(mod, "CreateInterface");
    printf("CreateInterface export = %p\n", (void *)create);
    if (!create) return 2;

    int rc = -1;
    void *client = create("SteamClient023", &rc);
    printf("CreateInterface(\"SteamClient023\") = %p rc=%d\n", client, rc);
    if (!client) return 3;

    /* ISteamClient vtable slot 0 is CreateSteamPipe; 0 means no running Steam to talk to. */
    CreateSteamPipeFn create_pipe = (*(CreateSteamPipeFn **)client)[0];
    int pipe = create_pipe(client);
    printf("ISteamClient::CreateSteamPipe() = %d\n", pipe);
    return 0;
}
