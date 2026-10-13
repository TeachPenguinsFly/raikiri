import { useEffect, useRef } from 'react'

export interface LightningProps {
  /** degrees, 0..360 */
  hue?: number
  speed?: number
  intensity?: number
  bolts?: number
  className?: string
}

const k_max_bolts = 8

const vertexSource = /* glsl */ `
attribute vec2 aCorner;
void main() {
  gl_Position = vec4(aCorner, 0.0, 1.0);
}
`

const fragmentSource = /* glsl */ `
// highp: the sine hash repeats visibly at mediump
precision highp float;
uniform vec2 uResolution;
uniform float uTime;
uniform float uHue;
uniform float uSpeed;
uniform float uIntensity;
uniform float uBolts;

float hash(float n) {
  return fract(sin(n * 12.9898) * 43758.5453);
}

float hash(vec2 q) {
  return hash(dot(q, vec2(1.0, 57.0)));
}

float lattice(vec2 q) {
  vec2 i = floor(q);
  vec2 f = fract(q);
  vec2 w = f * f * (3.0 - 2.0 * f);
  float bottom = mix(hash(i), hash(i + vec2(1.0, 0.0)), w.x);
  float top = mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), w.x);
  return mix(bottom, top, w.y);
}

// turned between octaves so the lattice grid does not show
float turbulence(vec2 q) {
  float sum = 0.0;
  float amp = 0.5;
  for (int k = 0; k < 8; ++k) {
    sum += amp * lattice(q);
    q = mat2(1.6, 1.2, -1.2, 1.6) * q;
    amp *= 0.5;
  }
  return sum;
}

vec3 tint(float hue) {
  vec3 rgb = 0.5 + 0.5 * cos(6.28318 * (hue + vec3(0.0, -0.33333, 0.33333)));
  return mix(vec3(1.0), rgb, 0.85);
}

void main() {
  vec2 p = (2.0 * gl_FragCoord.xy - uResolution) / uResolution.y;
  float px = 2.0 / uResolution.y;
  float t = uTime * uSpeed;
  float glow = 0.0;
  float core = 0.0;
  for (int b = 0; b < ${k_max_bolts}; ++b) {
    float fb = float(b);
    if (fb >= uBolts) break;
    float lane = mix(-0.7, 0.7, (fb + 0.5) / uBolts) + 0.2 * sin(t * 0.3 + fb * 2.3);
    // bending the plane, not the line, is what lets a bolt loop back and fork
    vec2 q = p * 0.9 + vec2(fb * 4.7, fb * 1.3) + vec2(0.35, 0.2) * t * (0.8 + 0.4 * hash(fb));
    float d = abs(p.y + 1.6 * (turbulence(q + vec2(5.2, 1.3)) - 0.5) - lane);
    float flash = 0.3 + 0.7 * hash(floor(t * (6.0 + 3.0 * hash(fb + 0.4))) + fb * 17.0);
    glow += flash * 0.006 / (d + 0.004);
    core += flash * exp(-0.5 * (d / (1.4 * px)) * (d / (1.4 * px)));
  }
  vec3 light = (tint(uHue / 360.0) * glow + vec3(0.9, 0.95, 1.0) * core * 0.6) * uIntensity;
  light = clamp(light, 0.0, 1.0);
  // premultiplied: the canvas adds light over whatever is behind it
  gl_FragColor = vec4(light, max(light.r, max(light.g, light.b)));
}
`

function shader(gl: WebGLRenderingContext, type: number, source: string): WebGLShader | null {
  const out = gl.createShader(type)
  if (out === null) return null
  gl.shaderSource(out, source)
  gl.compileShader(out)
  if (gl.getShaderParameter(out, gl.COMPILE_STATUS) !== true) {
    gl.deleteShader(out)
    return null
  }
  return out
}

function program(gl: WebGLRenderingContext): WebGLProgram | null {
  const vertex = shader(gl, gl.VERTEX_SHADER, vertexSource)
  const fragment = shader(gl, gl.FRAGMENT_SHADER, fragmentSource)
  const linked = gl.createProgram()
  if (vertex === null || fragment === null || linked === null) return null
  gl.attachShader(linked, vertex)
  gl.attachShader(linked, fragment)
  gl.linkProgram(linked)
  return gl.getProgramParameter(linked, gl.LINK_STATUS) === true ? linked : null
}

/** horizontal bolts in one webgl pass; the drawing buffer is capped and css scales it */
export default function Lightning({ hue = 199, speed = 1, intensity = 1, bolts = 5, className }: LightningProps) {
  const host = useRef<HTMLDivElement>(null)
  const live = useRef({ hue, speed, intensity, bolts })
  live.current = { hue, speed, intensity, bolts }

  useEffect(() => {
    const parent = host.current
    if (parent === null) return

    // a fresh canvas per mount: strictmode mounts twice and the cleanup loses the context
    const canvas = document.createElement('canvas')
    canvas.style.cssText = 'display:block;width:100%;height:100%'
    parent.appendChild(canvas)
    const detach = () => canvas.remove()

    // premultipliedAlpha stays on; chrome composites an unpremultiplied canvas as opaque white
    const gl = canvas.getContext('webgl', { alpha: true, antialias: false, powerPreference: 'low-power' })
    const linked = gl === null ? null : program(gl)
    if (gl === null || linked === null) return detach

    gl.useProgram(linked)
    gl.clearColor(0, 0, 0, 0)
    gl.enable(gl.BLEND)
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA)

    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer())
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW)
    const corner = gl.getAttribLocation(linked, 'aCorner')
    gl.enableVertexAttribArray(corner)
    gl.vertexAttribPointer(corner, 2, gl.FLOAT, false, 0, 0)

    const uniform = (name: string) => gl.getUniformLocation(linked, name)
    const u = {
      resolution: uniform('uResolution'),
      time: uniform('uTime'),
      hue: uniform('uHue'),
      speed: uniform('uSpeed'),
      intensity: uniform('uIntensity'),
      bolts: uniform('uBolts'),
    }

    const k_max_side = 1600
    const fit = () => {
      const scale = Math.min(1, k_max_side / Math.max(parent.clientWidth, parent.clientHeight, 1))
      canvas.width = Math.max(1, Math.round(parent.clientWidth * scale))
      canvas.height = Math.max(1, Math.round(parent.clientHeight * scale))
      gl.viewport(0, 0, canvas.width, canvas.height)
      gl.uniform2f(u.resolution, canvas.width, canvas.height)
    }
    const observer = new ResizeObserver(fit)
    observer.observe(parent)
    fit()

    const start = performance.now()
    const paint = () => {
      const p = live.current
      gl.uniform1f(u.time, (performance.now() - start) / 1000)
      gl.uniform1f(u.hue, p.hue)
      gl.uniform1f(u.speed, p.speed)
      gl.uniform1f(u.intensity, p.intensity)
      gl.uniform1f(u.bolts, Math.min(Math.max(1, Math.round(p.bolts)), k_max_bolts))
      gl.clear(gl.COLOR_BUFFER_BIT)
      gl.drawArrays(gl.TRIANGLES, 0, 3)
    }

    // the first frame now: a throttled tab may never schedule one
    paint()
    let frame = requestAnimationFrame(function loop() {
      frame = requestAnimationFrame(loop)
      if (!document.hidden) paint()
    })

    return () => {
      cancelAnimationFrame(frame)
      observer.disconnect()
      detach()
      gl.getExtension('WEBGL_lose_context')?.loseContext()
    }
  }, [])

  return <div ref={host} className={className} style={{ width: '100%', height: '100%' }} />
}
