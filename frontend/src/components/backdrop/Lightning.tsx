import { useEffect, useRef } from 'react'

/** A bolt, drawn as a fragment shader on a raw WebGL canvas.
 *
 *  Ported from React Bits with four corrections, all of them about cost:
 *
 *   - the original measured the canvas EVERY FRAME, which reads clientWidth
 *     and forces the browser to lay out sixty times a second for a number that
 *     changes when the window does. It resizes on resize now;
 *   - the loop stands down when the tab is hidden. A shader running ten
 *     octaves of fbm per pixel behind a background nobody is looking at is a
 *     fan spinning for nothing;
 *   - the context is released on unmount. Browsers cap how many live WebGL
 *     contexts a page may hold, and a route that mounts and unmounts would
 *     eventually be refused one;
 *   - props update uniforms instead of rebuilding the context, so nudging the
 *     hue does not throw away the program and compile it again.
 *
 *  It renders at CSS pixels rather than device pixels ON PURPOSE. The effect
 *  is a soft glow with no edge to alias, and rendering a ten-octave shader at
 *  4x the pixels on a retina panel buys nothing anybody can see. */
export interface LightningProps {
  /** Degrees, 0..360. */
  hue?: number
  xOffset?: number
  speed?: number
  intensity?: number
  size?: number
  /** How much air the bolt lights up around itself. */
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
// highp, not mediump. The hash functions below lean on the fractional bits of
// products like fract(p * .1031); at medium precision they collapse toward a
// constant, the distance term goes to zero everywhere and the bolt floods the
// frame white. Desktop GPUs quietly hand out highp anyway — declaring it is
// what makes software rasterisers and older mobile parts agree with them.
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

    // THE NOISE IS SAMPLED IN A SQUARE SPACE, not in the stretched one.
    //
    // uv.x was already multiplied by the aspect, so on a 4:1 window it runs to
    // ±11.6 — and feeding that straight to fbm samples the noise at four times
    // the frequency along the very axis the bolt travels. The line stops being
    // a line and breaks into a scatter of dots. Undoing the stretch for the
    // noise lookup keeps the wander the same shape at any window proportion;
    // the distance below still uses the stretched uv, so the bolt still spans
    // the full width.
    float aspect = max(iResolution.x / iResolution.y, 1.0);
    vec2 nuv = vec2(uv.x / aspect, uv.y);
    uv += 2.0 * fbm(nuv * uSize + 0.8 * iTime * uSpeed) - 1.0;

    // HORIZONTAL, and it fills the frame.
    //
    // The original takes the distance from uv.x, which puts the bolt on a
    // vertical line. On a wide screen uv.x has already been stretched by the
    // aspect to ±1.77 while the noise only displaces by ±1, so the bolt could
    // never leave the middle half — the narrow column it was stuck in. Taking
    // the distance from uv.y instead runs it across the frame, and there the
    // ±1 of wander is the whole height.
    float dist = abs(uv.y);
    vec3 baseColor = hsv2rgb(vec3(uHue / 360.0, 0.7, 0.8));

    // Two falloffs. The sharp one is the bolt; the wide one is the air around
    // it, and that is what makes a frame look LIT rather than drawn on. A
    // single 1/dist term gives a bright filament in the dark and no atmosphere.
    float core = mix(0.0, 0.07, hash11(iTime * uSpeed)) / dist;
    float haze = uGlow / (dist * dist + 0.22);
    vec3 col = baseColor * (core + haze) * uIntensity;
    float a = clamp(max(col.r, max(col.g, col.b)), 0.0, 1.0);
    // Premultiplied: the colour is carried through by its own alpha rather
    // than left for the compositor to apply.
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
  // Truthiness, NOT `!== true`. getShaderParameter is specified to hand back a
  // GLboolean, but implementations return a truthy value that is not the
  // boolean `true` — and a strict comparison then reports every shader as
  // broken, including a three-line vertex shader that cannot be. The symptom
  // is a canvas that never draws and composites as an opaque white flood.
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

    // THE CANVAS IS CREATED HERE, not rendered by React.
    //
    // `getContext` hands back the SAME context for the life of a canvas
    // element. The cleanup below deliberately loses that context — browsers cap
    // how many live ones a page may hold — and under StrictMode React mounts,
    // cleans up and mounts again, so a React-owned canvas would be handed its
    // own corpse on the second pass: every shader fails to compile and
    // getShaderInfoLog returns null, because there is no context left to ask.
    // A canvas made per mount gets a live context every time.
    const canvas = document.createElement('canvas')
    canvas.style.width = '100%'
    canvas.style.height = '100%'
    canvas.style.display = 'block'
    host.appendChild(canvas)

    // NOT `premultipliedAlpha: false`. The original asks for unpremultiplied
    // output, and Chrome then composites this canvas as opaque — the whole
    // backdrop floods white regardless of what the shader writes. Premultiplied
    // is the default and is what the compositor actually wants; the shader
    // multiplies the colour through below to match.
    const gl = canvas.getContext('webgl', {
      alpha: true,
      // Do not wake a discrete GPU for a background. On a laptop that is the
      // difference between a warm fan and a quiet one.
      powerPreference: 'low-power',
      // Nothing reads this buffer back between frames, and keeping it costs a
      // second full-screen allocation.
      preserveDrawingBuffer: false,
      antialias: false,
    })
    // No WebGL means no backdrop. The screen behind it is complete on its own,
    // so this degrades to nothing rather than to a console full of errors.
    if (gl === null) return

    // The original set neither. Without BLEND the fragment alpha is written
    // straight into the drawing buffer instead of compositing over what is
    // behind, and without a clear the buffer keeps whatever it held — which on
    // a fresh context is undefined, and shows up as an opaque flood.
    gl.clearColor(0, 0, 0, 0)
    gl.enable(gl.BLEND)
    // Premultiplied blending, to match the buffer the compositor expects.
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA)

    const vertex = compile(gl, vertexSource, gl.VERTEX_SHADER)
    const fragment = compile(gl, fragmentSource, gl.FRAGMENT_SHADER)
    if (vertex === null || fragment === null) return

    const program = gl.createProgram()
    if (program === null) return
    gl.attachShader(program, vertex)
    gl.attachShader(program, fragment)
    gl.linkProgram(program)
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return
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

    // THE COST OF THIS SHADER IS ITS PIXEL COUNT. It runs ten octaves of noise
    // per fragment, so a 2560-wide window is four times the work of a
    // 1280-wide one for an effect that is a soft glow with no edge to sharpen.
    // The buffer is capped and CSS scales it back up; nobody can see the
    // difference, and the drawing buffer stops being the largest allocation on
    // the page.
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

    // Time starts part-way in, not at zero. hash11(0) is 0, so at t = 0 the
    // brightness term is 0 and the very first frame is blank — the backdrop
    // would fade up from nothing on every load instead of already being there.
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

    // The first frame is drawn NOW rather than at the next animation frame.
    // Waiting for one means the backdrop is blank for however long it takes the
    // scheduler to get round to it — which on a throttled or headless page can
    // be forever, and the screen shows an undrawn canvas instead of nothing.
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
      if (host.contains(canvas)) host.removeChild(canvas)
      gl.getExtension('WEBGL_lose_context')?.loseContext()
    }
  }, [])

  return <div ref={ref} className={className} style={{ width: '100%', height: '100%' }} />
}
