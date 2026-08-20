const exportDllPath = "C:\\Users\\Vintendo\\.codex\\visualizations\\2026\\08\\15\\01a006ed-09d5-7162-9813-b955b1cf1bd9\\BigscreenDesktopBridge\\runtime_camera_probe\\v22a-native\\build\\Release\\BigscreenThirdPersonGpuExport.dll";
const exportReportPath = "C:\\Users\\Vintendo\\AppData\\Local\\Temp\\BigscreenV24-GPU.txt";
const textValue = o => typeof o === "string" ? o : (o?.content ?? "");

Il2Cpp.perform(() => {
    const core = Il2Cpp.domain.assembly("UnityEngine.CoreModule").image;
    const findClass = (ns, name) => core.classes.find(c => c.namespace === ns && c.name === name);
    const allClasses = () => Il2Cpp.domain.assemblies.flatMap(a => a.image.classes);
    const goClass = findClass("UnityEngine", "GameObject");
    const resources = findClass("UnityEngine", "Resources");
    const cameraClass = findClass("UnityEngine", "Camera");
    const componentClass = findClass("UnityEngine", "Component");
    const rtClass = findClass("UnityEngine", "RenderTexture");
    const texClass = findClass("UnityEngine", "Texture");
    const vec3Class = findClass("UnityEngine", "Vector3");
    const quatClass = findClass("UnityEngine", "Quaternion");
    const captureClass = allClasses().find(c => c.namespace === "Bigscreen.CaptureMode" && c.name === "CaptureCamera");
    const controllerClass = allClasses().find(c => c.namespace === "Bigscreen.CaptureMode" && c.name === "CaptureCameraController");
    const late = captureClass?.methods.find(m => m.name === "LateUpdate" && m.parameterCount === 0);
    const controllerUpdate = controllerClass?.methods.find(m => m.name === "Update" && m.parameterCount === 0);
    if (!goClass || !resources || !cameraClass || !componentClass || !rtClass || !texClass || !vec3Class || !quatClass || !captureClass || !late || !controllerUpdate) { console.log("V24 STOP required metadata missing"); return; }
    console.log(`V24 metadata CaptureCamera.LateUpdate=${late} Controller.Update=${controllerUpdate}`);

    const findAll = resources.methods.find(m => m.name === "FindObjectsOfTypeAll" && m.parameterCount === 1);
    const objects = findAll.invoke(goClass.type.object);
    let captureGo = null, captureCamera = null, avatarGo = null, avatarTransform = null;
    for (let i = 0; i < objects.length; i++) {
        try {
            const go = objects.get(i); if (!go || !go.handle || go.handle.isNull() || go.class.name !== "GameObject") continue;
            const name = textValue(go.method("get_name").invoke());
            if (name !== "CaptureCamera" && name !== "LocalAvatar_Stronk(Clone)") continue;
            const components = go.method("GetComponents").overload("System.Type").invoke(componentClass.type.object);
            const names = [...components].filter(Boolean).map(c => `${c.class.namespace}.${c.class.name}`);
            if (name === "CaptureCamera" && names.includes("Bigscreen.CaptureMode.CaptureCamera")) { captureGo = go; captureCamera = go.method("GetComponent").overload("System.Type").invoke(cameraClass.type.object); }
            if (name === "LocalAvatar_Stronk(Clone)") { avatarGo = go; avatarTransform = go.method("get_transform").invoke(); }
        } catch (_) {}
    }
    if (!captureGo || !captureCamera || !avatarGo || !avatarTransform) { console.log(`V24 STOP objects capture=${!!captureGo} avatar=${!!avatarGo}`); return; }
    console.log(`V24 live objects captureGo=${captureGo.handle} avatar=${avatarGo.handle}`);

    const zeroObject = () => new Il2Cpp.Object(ptr(0));
    const rtGetTemporary = rtClass.methods.find(m => m.name === "GetTemporary" && m.parameterCount === 3);
    const rtReleaseTemporary = rtClass.methods.find(m => m.name === "ReleaseTemporary" && m.parameterCount === 1);
    const getNative = texClass.methods.find(m => m.name === "GetNativeTexturePtr" && m.parameterCount === 0);
    const setTarget = captureCamera.method("set_targetTexture");
    const camTransform = captureGo.method("get_transform").invoke();
    const lookRotation = quatClass.methods.find(m => m.name === "LookRotation" && m.parameterCount === 2);
    const posMem = Il2Cpp.alloc(12); const upMem = Il2Cpp.alloc(12); const forwardMem = Il2Cpp.alloc(12);
    const posValue = new Il2Cpp.ValueType(posMem, vec3Class.type); const upValue = new Il2Cpp.ValueType(upMem, vec3Class.type); const forwardValue = new Il2Cpp.ValueType(forwardMem, vec3Class.type);
    const writeVec = (mem, v) => { mem.add(0).writeFloat(v.x); mem.add(4).writeFloat(v.y); mem.add(8).writeFloat(v.z); };
    const readVec = v => ({ x: v.field("x").value, y: v.field("y").value, z: v.field("z").value });
    const add = (a,b) => ({x:a.x+b.x,y:a.y+b.y,z:a.z+b.z});
    const mul = (a,s) => ({x:a.x*s,y:a.y*s,z:a.z*s});
    const sub = (a,b) => ({x:a.x-b.x,y:a.y-b.y,z:a.z-b.z});

    const module = Module.load(exportDllPath);
    const initExport = new NativeFunction(module.getExportByName("InitThirdPersonGpu"), "int", ["pointer", "pointer"]);
    const copyExport = new NativeFunction(module.getExportByName("CopyThirdPersonGpu"), "int", ["pointer"]);
    const shutdownExport = new NativeFunction(module.getExportByName("ShutdownThirdPersonGpu"), "int", []);
    let rt = null, initialized = false, frame = 0, activeChanged = false, lateHook = null, updateHook = null, shuttingDown = false;
    const cleanup = () => {
        if (shuttingDown) return; shuttingDown = true;
        try { if (captureGo) captureGo.method("SetActive").invoke(false); } catch (_) {}
        try { if (captureCamera) setTarget.invoke(zeroObject()); } catch (_) {}
        try { shutdownExport(); } catch (_) {}
        try { if (rt) rtReleaseTemporary.invoke(rt); } catch (_) {}
        try { lateHook?.detach(); updateHook?.detach(); } catch (_) {}
        console.log("V24 cleanup complete");
    };
    updateHook = Interceptor.attach(controllerUpdate.virtualAddress, { onLeave() {
        if (activeChanged || shuttingDown) return;
        activeChanged = true;
        try {
            if (!captureGo.method("get_activeSelf").invoke()) captureGo.method("SetActive").invoke(true);
            rt = rtGetTemporary.invoke(1280, 720, 24); rt.method("Create").invoke();
            if (!rt.method("IsCreated").invoke()) throw new Error("temporary RT not created");
            setTarget.invoke(rt);
            const native = rt.method("GetNativeTexturePtr").invoke();
            const p = native.handle ?? native;
            const rc = initExport(p, Memory.allocUtf16String(exportReportPath));
            if (rc !== 0) throw new Error(`GPU export init rc=${rc}`);
            initialized = true; console.log(`V24 CaptureCamera activated RT=${rt.handle} native=${p} export initialized rc=${rc}`);
        } catch (e) { console.log(`V24 activation error=${e}`); cleanup(); }
        updateHook?.detach();
    }});
    lateHook = Interceptor.attach(late.virtualAddress, { onLeave() {
        if (!initialized || shuttingDown) return;
        try {
            const avatarPos = readVec(avatarTransform.method("get_position").invoke());
            const avatarForward = readVec(avatarTransform.method("get_forward").invoke());
            const avatarUp = readVec(avatarTransform.method("get_up").invoke());
            const target = add(avatarPos, mul(avatarUp, 1.15));
            const cameraPos = add(sub(target, mul(avatarForward, 2.5)), mul(avatarUp, 0.6));
            const direction = sub(target, cameraPos);
            writeVec(posMem, cameraPos); writeVec(upMem, avatarUp); writeVec(forwardMem, direction);
            camTransform.method("set_position").invoke(posValue);
            const rotation = lookRotation.invoke(forwardValue, upValue);
            camTransform.method("set_rotation").invoke(rotation);
            const native = rt.method("GetNativeTexturePtr").invoke(); const p = native.handle ?? native;
            const rc = copyExport(p); if (rc !== 0 && frame % 60 === 0) console.log(`V24 export copy rc=${rc}`);
            if (++frame % 60 === 0) console.log(`V24 live GPU frames=${frame} mainTid=${Process.getCurrentThreadId()} camera=${cameraPos.x.toFixed(2)},${cameraPos.y.toFixed(2)},${cameraPos.z.toFixed(2)}`);
        } catch (e) { console.log(`V24 frame error=${e}`); cleanup(); }
    }});
    console.log("V24 GPU-only hooks installed; waiting for activation");
    setTimeout(cleanup, 120000);
});
