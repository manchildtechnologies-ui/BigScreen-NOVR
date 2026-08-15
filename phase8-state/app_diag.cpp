#include <openvr.h>
#include <cstdio>

int main() {
    vr::EVRInitError e = vr::VRInitError_None;
    if (!vr::VR_Init(&e, vr::VRApplication_Utility)) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
        return 2;
    }
    auto* apps = vr::VRApplications();
    if (!apps) { std::printf("VRApplications unavailable\n"); vr::VR_Shutdown(); return 3; }
    std::printf("CurrentScenePID=%u\n", apps->GetCurrentSceneProcessId());
    const uint32_t n = apps->GetApplicationCount();
    std::printf("ApplicationCount=%u\n", n);
    for (uint32_t i=0; i<n; ++i) {
        char key[vr::k_unMaxApplicationKeyLength]{};
        if (apps->GetApplicationKeyByIndex(i,key,sizeof(key)) != vr::VRApplicationError_None) continue;
        char name[256]{}; vr::EVRApplicationError pe=vr::VRApplicationError_None;
        apps->GetApplicationPropertyString(key,vr::VRApplicationProperty_Name_String,name,sizeof(name),&pe);
        std::printf("key=%s pid=%u name=%s nameError=%d\n",key,apps->GetApplicationProcessId(key),name,(int)pe);
    }
    vr::VR_Shutdown();
    return 0;
}
