import { useEffect, useRef } from 'react'

/** webgl lightning backdrop ported from react bits; renders at css pixels on purpose */
export interface LightningProps {
  /** degrees, 0..360 */
  hue?: number
  xOffset?: number
  speed?: number
  intensity?: number
  size?: number
  glow?: number
  className?: string
}

const vertexSource = /* glsl */ `
attribute vec2 aPosition;
void main() {
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
`

const fragmentSource = /* glsl */ `
// highp: the hash functions collapse at mediump and the frame floods white
precision highp float;
uniform vec2 iResolution;
uniform float iTime;
uniform float uHue;
uniform float uXOffset;
uniform float uSpeed;
uniform float uIntensity;
uniform float uSize;
uniform float uGlow;

#define OCTAVE_COUNT 10

vec3 hsv2rgb(vec3 c) {
    vec3 rgb = clamp(abs(mod(c.x * 6.0 + vec3(0.0,4.0,2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z * mix(vec3(1.0), rgb, c.y);
}

float hash11(float p) {
    p = fract(p * .1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

mat2 rotate2d(float theta) {
    float c = cos(theta);
    float s = sin(theta);
    return mat2(c, -s, s, c);
}

float noise(vec2 p) {
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    float a = hash12(ip);
    float b = hash12(ip + vec2(1.0, 0.0));
    float c = hash12(ip + vec2(0.0, 1.0));
    float d = hash12(ip + vec2(1.0, 1.0));

    vec2 t = smoothstep(0.0, 1.0, fp);
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < OCTAVE_COUNT; ++i) {
        value += amplitude * noise(p);
        p *= rotate2d(0.45);
        p *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    uv = 2.0 * uv - 1.0;
    uv.x *= iResolution.x / iResolution.y;
    uv.x += uXOffset;

    // sample noise in unstretched space so the bolt keeps its shape at any aspect
    float aspect = max(iResolution.x / iResolution.y, 1.0);
    vec2 nuv = vec2(uv.x / aspect, uv.y);
    uv += 2.0 * fbm(nuv * uSize + 0.8 * iTime * uSpeed) - 1.0;

    // distance from uv.y runs the bolt horizontally
    float dist = abs(uv.y);
    vec3 baseColor = hsv2rgb(vec3(uHue / 360.0, 0.7, 0.8));

    float core = mix(0.0, 0.07, hash11(iTime * uSpeed)) / dist;
    float haze = uGlow / (dist * dist + 0.22);
    vec3 col = baseColor * (core + haze) * uIntensity;
    float a = clamp(max(col.r, max(col.g, col.b)), 0.0, 1.0);
    fragColor = vec4(clamp(col, 0.0, 1.0) * a, a);
}

void main() {
    mainImage(gl_FragColor, gl_FragCoord.xy);
}
`

function compile(gl: WebGLRenderingContext, source: string, type: number): WebGLShader | null {
  const shader = gl.createShader(type)
  if (shader === null) return null
  gl.shaderSource(shader, source)
  gl.compileShader(shader)
  // truthiness check: some implementations return a truthy non-boolean
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    gl.deleteShader(shader)
    return null
  }
  return shader
}

export default function Lightning({
  hue = 230,
  xOffset = 0,
  speed = 1,
  intensity = 1,
  size = 1,
  glow = 0.05,
  className,
}: LightningProps) {
  const ref = useRef<HTMLDivElement>(null)
  const props = useRef({ hue, xOffset, speed, intensity, size, glow })
  props.current = { hue, xOffset, speed, intensity, size, glow }

  useEffect(() => {
    const host = ref.current
    if (host === null) return

    // a canvas per mount: the cleanup loses the context and strictmode mounts twice
    const canvas = document.createElement('canvas')
    canvas.style.width = '100%'
    canvas.style.height = '100%'
    canvas.style.display = 'block'
    host.appendChild(canvas)

    // keep premultiplied alpha; chrome composites an unpremultiplied canvas as opaque white
    const gl = canvas.getContext('webgl', {
      alpha: true,
      powerPreference: 'low-power',
      preserveDrawingBuffer: false,
      antialias: false,
    })
    const drop = () => {
      if (host.contains(canvas)) host.removeChild(canvas)
    }
    if (gl === null) return drop

    // blend and a transparent clear, or the fresh buffer composites opaque
    gl.clearColor(0, 0, 0, 0)
    gl.enable(gl.BLEND)
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA)

    const vertex = compile(gl, vertexSource, gl.VERTEX_SHADER)
    const fragment = compile(gl, fragmentSource, gl.FRAGMENT_SHADER)
    if (vertex === null || fragment === null) return drop

    const program = gl.createProgram()
    if (program === null) return drop
    gl.attachShader(program, vertex)
    gl.attachShader(program, fragment)
    gl.linkProgram(program)
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return drop
    gl.useProgram(program)

    const buffer = gl.createBuffer()
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]),
      gl.STATIC_DRAW,
    )
    const position = gl.getAttribLocation(program, 'aPosition')
    gl.enableVertexAttribArray(position)
    gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0)

    const at = (name: string) => gl.getUniformLocation(program, name)
    const uniforms = {
      resolution: at('iResolution'),
      time: at('iTime'),
      hue: at('uHue'),
      xOffset: at('uXOffset'),
      speed: at('uSpeed'),
      intensity: at('uIntensity'),
      size: at('uSize'),
      glow: at('uGlow'),
    }

    // the shader costs per pixel, so the buffer is capped and css scales it up
    const k_max_dim = 1600
    const resize = () => {
      const w = host.clientWidth
      const h = host.clientHeight
      const scale = Math.min(1, k_max_dim / Math.max(w, h, 1))
      canvas.width = Math.max(1, Math.round(w * scale))
      canvas.height = Math.max(1, Math.round(h * scale))
      gl.viewport(0, 0, canvas.width, canvas.height)
      gl.uniform2f(uniforms.resolution, canvas.width, canvas.height)
    }
    const observer = new ResizeObserver(resize)
    observer.observe(host)
    resize()

    // hash11(0) is 0, so starting at t = 0 draws a blank first frame
    const started = performance.now() - 4200
    let frame = 0
    const draw = () => {
      const live = props.current
      gl.uniform1f(uniforms.time, (performance.now() - started) / 1000)
      gl.uniform1f(uniforms.hue, live.hue)
      gl.uniform1f(uniforms.xOffset, live.xOffset)
      gl.uniform1f(uniforms.speed, live.speed)
      gl.uniform1f(uniforms.intensity, live.intensity)
      gl.uniform1f(uniforms.size, live.size)
      gl.uniform1f(uniforms.glow, live.glow)
      gl.clear(gl.COLOR_BUFFER_BIT)
      gl.drawArrays(gl.TRIANGLES, 0, 6)
    }

    // draw the first frame now; a throttled page may never schedule one
    draw()
    const render = () => {
      frame = requestAnimationFrame(render)
      if (document.hidden) return
      draw()
    }
    frame = requestAnimationFrame(render)

    return () => {
      cancelAnimationFrame(frame)
      observer.disconnect()
      drop()
      gl.getExtension('WEBGL_lose_context')?.loseContext()
    }
  }, [])

  return <div ref={ref} className={className} style={{ width: '100%', height: '100%' }} />
}
