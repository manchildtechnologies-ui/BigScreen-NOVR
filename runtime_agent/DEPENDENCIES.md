# Embedded agent dependencies

- Frida Core devkit: 17.17.0 (native static host; official Windows x64 devkit)
- frida-il2cpp-bridge: 0.13.1 (bundled into `dist/third_person_agent.js`)
- frida-compile: 19.0.5 (build-time only)
- Frida Node binding: 17.17.0 (build-time only, not shipped)

The installed runtime contains the native host, the compiled agent, the GPU exporter,
and the license notices. It does not require Python, Node.js, npm, frida-tools, or
an external `frida.exe`.
