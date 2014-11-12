#include "postcompositor-rift.h"
#include "compositor.h"
#include "gl-renderer.h"
#include <wayland-server.h>
#include <GLES2/gl2.h>
#include <ovr-0.4.3/Src/OVR_CAPI.h>
#include <linux/input.h>

// Rift shaders
// Distortion shader

static const char* distortion_vertex_shader =
  "uniform vec2 EyeToSourceUVScale;\n"
  "uniform vec2 EyeToSourceUVOffset;\n"
  "uniform bool RightEye;\n"
  "uniform float angle;\n"
  "attribute vec2 Position;\n"
  "attribute vec2 TexCoord0;\n"
  "varying mediump vec2 oTexCoord0;\n"
  "attribute vec2 TexCoordR;\n"
  //"attribute vec2 TexCoordG;\n"
  "attribute vec2 TexCoordB;\n"
  "varying mediump vec2 oTexCoordR;\n"
  //"varying mediump vec2 oTexCoordG;\n"
  "varying mediump vec2 oTexCoordB;\n"
  "vec2 tanEyeAngleToTexture(vec2 v) {\n"
  "  vec2 result = v * EyeToSourceUVScale + EyeToSourceUVOffset;\n"
  "  result.y = 1.0 - result.y;\n"
  "  return result;\n"
  "}\n"
  "void main() {\n"
  "  oTexCoord0 = tanEyeAngleToTexture(TexCoord0);\n"
  "  oTexCoordR = tanEyeAngleToTexture(TexCoordR);\n"
  //"  oTexCoordG = tanEyeAngleToTexture(TexCoordG);\n"
  "  oTexCoordB = tanEyeAngleToTexture(TexCoordB);\n"
  "  vec2 b = Position;\n"
  "  b.x = Position.x*cos(angle) - Position.y*sin(angle);\n"
  "  b.y = Position.y*cos(angle) + Position.x*sin(angle);\n"
  "  gl_Position.xy = b;\n"
  "  gl_Position.z = 0.5;\n"
  "  gl_Position.w = 1.0;\n"
  "}\n";

static const char* distortion_fragment_shader =
  "varying mediump vec2 oTexCoord0;\n"
  "varying mediump vec2 oTexCoordR;\n"
  //"varying mediump vec2 oTexCoordG;\n"
  "varying mediump vec2 oTexCoordB;\n"
  "uniform sampler2D Texture0;\n"
  "void main() {\n"
/*  "  gl_FragColor.r = texture2D(Texture0, oTexCoordR).r;\n"
  "  gl_FragColor = texture2D(Texture0, oTexCoord0);\n"
  "  gl_FragColor.a = 1.0;\n"
  "  gl_FragColor.g = texture2D(Texture0, oTexCoordG).g;\n"
  "  gl_FragColor.b = texture2D(Texture0, oTexCoordB).b;\n"*/
  "  mediump float r = texture2D(Texture0, oTexCoordR).r;\n"
  "  mediump float g = texture2D(Texture0, oTexCoord0).g;\n"
  "  mediump float b = texture2D(Texture0, oTexCoordB).b;\n"
  "  gl_FragColor = vec4(r, g, b, 1.0);\n"
  "}\n";

// Rendered scene (per eye) shader

static const char* eye_vertex_shader = 
  "attribute vec3 Position;\n"
  "attribute vec2 TexCoord0;\n"
  "uniform mat4 Projection;\n"
  "uniform mat4 ModelView;\n"
  "varying mediump vec2 oTexCoord0;\n"
  "void main() {\n"
  "  oTexCoord0 = TexCoord0;\n"
  "  gl_Position = vec4(Position, 1.0) * ModelView * Projection;\n"
  //"  gl_Position = Projection * ModelView * vec4(Position, 1.0);\n"// * Projection;\n"
  "}\n";

static const char* eye_fragment_shader =
  "varying mediump vec2 oTexCoord0;\n"
  "uniform sampler2D Texture0;\n"
  "void main() {\n"
  "  gl_FragColor = texture2D(Texture0, oTexCoord0);\n"
  "}\n";

// End of shaders

// Matrix, Quaternion, and Vector math functions, using ovr types
// There are weston_matrix functions, maybe use those instead?

static inline ovrMatrix4f initIdentity(void)
{
  ovrMatrix4f r;

  r.M[0][0] = 1; r.M[0][1] = 0; r.M[0][2] = 0; r.M[0][3] = 0;
  r.M[1][0] = 0; r.M[1][1] = 1; r.M[1][2] = 0; r.M[1][3] = 0;
  r.M[2][0] = 0; r.M[2][1] = 0; r.M[2][2] = 1; r.M[2][3] = 0;
  r.M[3][0] = 0; r.M[3][1] = 0; r.M[3][2] = 0; r.M[3][3] = 1;

  return r;
}

static inline ovrMatrix4f initScale(float x, float y, float z)
{
  ovrMatrix4f r;

  r.M[0][0] = x; r.M[0][1] = 0; r.M[0][2] = 0; r.M[0][3] = 0;
  r.M[1][0] = 0; r.M[1][1] = y; r.M[1][2] = 0; r.M[1][3] = 0;
  r.M[2][0] = 0; r.M[2][1] = 0; r.M[2][2] = z; r.M[2][3] = 0;
  r.M[3][0] = 0; r.M[3][1] = 0; r.M[3][2] = 0; r.M[3][3] = 1;

  return r;
}

static inline ovrMatrix4f initTranslationF(float x, float y, float z)
{
  ovrMatrix4f r;

  r.M[0][0] = 1; r.M[0][1] = 0; r.M[0][2] = 0; r.M[0][3] = x;
  r.M[1][0] = 0; r.M[1][1] = 1; r.M[1][2] = 0; r.M[1][3] = y;
  r.M[2][0] = 0; r.M[2][1] = 0; r.M[2][2] = 1; r.M[2][3] = z;
  r.M[3][0] = 0; r.M[3][1] = 0; r.M[3][2] = 0; r.M[3][3] = 1;

  return r;
}

static inline ovrMatrix4f matrix4fMul(const ovrMatrix4f m1, const ovrMatrix4f m2)
{
  ovrMatrix4f result;
  int i, j;

  for(i=0; i<4; i++)
  {
    for(j=0; j<4; j++)
    {
      result.M[i][j] = 
        m1.M[i][0] * m2.M[0][j] +
        m1.M[i][1] * m2.M[1][j] +
        m1.M[i][2] * m2.M[2][j] +
        m1.M[i][3] * m2.M[3][j];
    }
  }

  return result;
}

static inline ovrMatrix4f quatfToMatrix4f(const ovrQuatf q)
{
  ovrMatrix4f m1, m2;

  m1.M[0][0] = q.w; m1.M[0][1] = q.z; m1.M[0][2] = -q.y; m1.M[0][3] = q.x;
  m1.M[1][0] = -q.z; m1.M[1][1] = q.w; m1.M[1][2] = q.x; m1.M[1][3] = q.y;
  m1.M[2][0] = q.y; m1.M[2][1] = -q.x; m1.M[2][2] = q.w; m1.M[2][3] = q.z;
  m1.M[3][0] = -q.x; m1.M[3][1] = -q.y; m1.M[3][2] = -q.z; m1.M[3][3] = q.w;

  m2.M[0][0] = q.w; m2.M[0][1] = q.z; m2.M[0][2] = -q.y; m2.M[0][3] = -q.x;
  m2.M[1][0] = -q.z; m2.M[1][1] = q.w; m2.M[1][2] = q.x; m2.M[1][3] = -q.y;
  m2.M[2][0] = q.y; m2.M[2][1] = -q.x; m2.M[2][2] = q.w; m2.M[2][3] = -q.z;
  m2.M[3][0] = q.x; m2.M[3][1] = q.y; m2.M[3][2] = q.z; m2.M[3][3] = q.w;

  return matrix4fMul(m1, m2);
}

static inline ovrMatrix4f initTranslation(const ovrVector3f position)
{
  ovrMatrix4f r;

  r.M[0][0] = 1; r.M[0][1] = 0; r.M[0][2] = 0; r.M[0][3] = position.x;
  r.M[1][0] = 0; r.M[1][1] = 1; r.M[1][2] = 0; r.M[1][3] = position.y;
  r.M[2][0] = 0; r.M[2][1] = 0; r.M[2][2] = 1; r.M[2][3] = position.z;
  r.M[3][0] = 0; r.M[3][1] = 0; r.M[3][2] = 0; r.M[3][3] = 1;

  return r;
}

/* // It turns out we don't need inverse or transpose just yet, we can just use
   // row-major methods (basically, just reverse order of matrix multiplication)

  static inline ovrMatrix4f inverse(const ovrMatrix4f matrix)
{
  float *m = (float *)&matrix.M;
  ovrMatrix4f result;
  float *invOut = (float *)&result.M;
  float inv[16], det;
  int i;
  
    inv[0] = m[5]  * m[10] * m[15] - 
             m[5]  * m[11] * m[14] - 
             m[9]  * m[6]  * m[15] + 
             m[9]  * m[7]  * m[14] +
             m[13] * m[6]  * m[11] - 
             m[13] * m[7]  * m[10];

    inv[4] = -m[4]  * m[10] * m[15] + 
              m[4]  * m[11] * m[14] + 
              m[8]  * m[6]  * m[15] - 
              m[8]  * m[7]  * m[14] - 
              m[12] * m[6]  * m[11] + 
              m[12] * m[7]  * m[10];

    inv[8] = m[4]  * m[9] * m[15] - 
             m[4]  * m[11] * m[13] - 
             m[8]  * m[5] * m[15] + 
             m[8]  * m[7] * m[13] + 
             m[12] * m[5] * m[11] - 
             m[12] * m[7] * m[9];

    inv[12] = -m[4]  * m[9] * m[14] + 
               m[4]  * m[10] * m[13] +
               m[8]  * m[5] * m[14] - 
               m[8]  * m[6] * m[13] - 
               m[12] * m[5] * m[10] + 
               m[12] * m[6] * m[9];

    inv[1] = -m[1]  * m[10] * m[15] + 
              m[1]  * m[11] * m[14] + 
              m[9]  * m[2] * m[15] - 
              m[9]  * m[3] * m[14] - 
              m[13] * m[2] * m[11] + 
              m[13] * m[3] * m[10];

    inv[5] = m[0]  * m[10] * m[15] - 
             m[0]  * m[11] * m[14] - 
             m[8]  * m[2] * m[15] + 
             m[8]  * m[3] * m[14] + 
             m[12] * m[2] * m[11] - 
             m[12] * m[3] * m[10];

    inv[9] = -m[0]  * m[9] * m[15] + 
              m[0]  * m[11] * m[13] + 
              m[8]  * m[1] * m[15] - 
              m[8]  * m[3] * m[13] - 
              m[12] * m[1] * m[11] + 
              m[12] * m[3] * m[9];

    inv[13] = m[0]  * m[9] * m[14] - 
              m[0]  * m[10] * m[13] - 
              m[8]  * m[1] * m[14] + 
              m[8]  * m[2] * m[13] + 
              m[12] * m[1] * m[10] - 
              m[12] * m[2] * m[9];

    inv[2] = m[1]  * m[6] * m[15] - 
             m[1]  * m[7] * m[14] - 
             m[5]  * m[2] * m[15] + 
             m[5]  * m[3] * m[14] + 
             m[13] * m[2] * m[7] - 
             m[13] * m[3] * m[6];

    inv[6] = -m[0]  * m[6] * m[15] + 
              m[0]  * m[7] * m[14] + 
              m[4]  * m[2] * m[15] - 
              m[4]  * m[3] * m[14] - 
              m[12] * m[2] * m[7] + 
              m[12] * m[3] * m[6];

    inv[10] = m[0]  * m[5] * m[15] - 
              m[0]  * m[7] * m[13] - 
              m[4]  * m[1] * m[15] + 
              m[4]  * m[3] * m[13] + 
              m[12] * m[1] * m[7] - 
              m[12] * m[3] * m[5];

    inv[14] = -m[0]  * m[5] * m[14] + 
               m[0]  * m[6] * m[13] + 
               m[4]  * m[1] * m[14] - 
               m[4]  * m[2] * m[13] - 
               m[12] * m[1] * m[6] + 
               m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + 
              m[1] * m[7] * m[10] + 
              m[5] * m[2] * m[11] - 
              m[5] * m[3] * m[10] - 
              m[9] * m[2] * m[7] + 
              m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - 
             m[0] * m[7] * m[10] - 
             m[4] * m[2] * m[11] + 
             m[4] * m[3] * m[10] + 
             m[8] * m[2] * m[7] - 
             m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + 
               m[0] * m[7] * m[9] + 
               m[4] * m[1] * m[11] - 
               m[4] * m[3] * m[9] - 
               m[8] * m[1] * m[7] + 
               m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - 
              m[0] * m[6] * m[9] - 
              m[4] * m[1] * m[10] + 
              m[4] * m[2] * m[9] + 
              m[8] * m[1] * m[6] - 
              m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
    {
      weston_log("Error, matrix inverse determinant is zero\n");
      return initIdentity();
    }

    det = 1.0 / det;

    for (i = 0; i < 16; i++)
        invOut[i] = inv[i] * det;

    return result;
}

static inline ovrMatrix4f transpose(const ovrMatrix4f m)
{
  ovrMatrix4f result;
  int i, j;

  for(i=0; i<4; i++)
  {
    for(j=0; j<4; j++)
    {
      result.M[i][j] = m.M[j][i];
    }
  }
  return result;
}*/

static inline ovrMatrix4f posefToMatrix4f(const ovrPosef pose)
{
  ovrMatrix4f orientation = quatfToMatrix4f(pose.Orientation);
  ovrMatrix4f translation = initTranslation(pose.Position);
  translation.M[0][3] = -translation.M[0][3];
  translation.M[1][3] = -translation.M[1][3];
  translation.M[2][3] = -translation.M[2][3];

  return matrix4fMul(translation, orientation);
}

// End of Matrix, Quaternion, and Vector math

int
config_rift(struct weston_compositor *compositor)
//config_rift(struct weston_compositor *compositor, EGLConfig egl_config, EGLDisplay egl_display, EGLSurface orig_surface, EGLContext egl_context)
{
  compositor->rift = calloc(1, sizeof *(compositor->rift));
  /*compositor->rift->egl_config = egl_config;
  compositor->rift->egl_display = egl_display;
  compositor->rift->orig_surface = orig_surface;
  compositor->rift->egl_context = egl_context;*/

  return 0;
}

void show_error_(const char *file, int line)
{
  GLenum error = GL_NO_ERROR;
  error = glGetError();
  if(error != GL_NO_ERROR)
  {
    switch(error)
    {
      case GL_INVALID_OPERATION: weston_log("\tGL Error: GL_INVALID_OPERATION - %s : %i\n", file, line); break;
      case GL_INVALID_ENUM: weston_log("\tGL Error: GL_INVALID_ENUM - %s : %i\n", file, line); break;
      case GL_INVALID_VALUE: weston_log("\tGL Error: GL_INVALID_VALUE - %s : %i\n", file, line); break;
      case GL_OUT_OF_MEMORY: weston_log("\tGL Error: GL_OUT_OF_MEMORY - %s : %i\n", file, line); break;
      case GL_INVALID_FRAMEBUFFER_OPERATION: weston_log("\tGL Error: GL_INVALID_FRAMEBUFFER_OPERATION - %s : %i\n", file, line); break;
    }
  }
}

#define show_error() show_error_(__FILE__,__LINE__)

static GLuint CreateShader(GLenum type, const char *shader_src)
{
  GLuint shader = glCreateShader(type);
  if(!shader)
    return 0;

  glShaderSource(shader, 1, &shader_src, NULL);
  glCompileShader(shader);

  GLint compiled = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if(!compiled)
  {
    GLint info_len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
    if(info_len > 1)
    {
      char *info_log = (char *)malloc(sizeof(char) * info_len);
      glGetShaderInfoLog(shader, info_len, NULL, info_log);
      weston_log("\tError compiling shader:\n\t%s\n", info_log);
      free(info_log);
    }
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint CreateProgram(const char *vertex_shader_src, const char *fragment_shader_src)
{
  GLuint vertex_shader = CreateShader(GL_VERTEX_SHADER, vertex_shader_src);
  if(!vertex_shader)
    return 0;
  GLuint fragment_shader = CreateShader(GL_FRAGMENT_SHADER, fragment_shader_src);
  if(!fragment_shader)
  {
    glDeleteShader(vertex_shader);
    return 0;
  }

  GLuint program_object = glCreateProgram();
  if(!program_object)
    return 0;
  glAttachShader(program_object, vertex_shader);
  glAttachShader(program_object, fragment_shader);

  glLinkProgram(program_object);

  GLint linked = 0;
  glGetProgramiv(program_object, GL_LINK_STATUS, &linked);
  if(!linked)
  {
    GLint info_len = 0;
    glGetProgramiv(program_object, GL_INFO_LOG_LENGTH, &info_len);
    if(info_len > 1)
    {
      char *info_log = (char *)malloc(info_len);
      glGetProgramInfoLog(program_object, info_len, NULL, info_log);
      weston_log("\tError linking program:\n\t%s\n", info_log);
      free(info_log);
    }
    glDeleteProgram(program_object);
    return 0;
  }

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  return program_object;
}

static void
toggle_sbs(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  if(compositor->rift->sbs == 1)
    compositor->rift->sbs = 0;
  else
    compositor->rift->sbs = 1;
}

static void
toggle_rotate(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  if(compositor->rift->rotate == 1)
    compositor->rift->rotate = 0;
  else
    compositor->rift->rotate = 1;
}

static void
move_in(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_z += 0.1;
}

static void
move_out(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_z -= 0.1;
}

static void
scale_up(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_scale += 0.1;
}

static void
scale_down(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
  struct weston_compositor *compositor = data;
  compositor->rift->screen_scale -= 0.1;
}

int
setup_rift(struct weston_compositor *compositor)
{
  struct oculus_rift *rift = compositor->rift;

  rift->enabled = 1;

  rift->screen_z = -5.0;
  rift->screen_scale = 1.0;

  weston_compositor_add_key_binding(compositor, KEY_5, MODIFIER_SUPER, 
      toggle_sbs, compositor);
  weston_compositor_add_key_binding(compositor, KEY_6, MODIFIER_SUPER, 
      toggle_rotate, compositor);
  weston_compositor_add_key_binding(compositor, KEY_7, MODIFIER_SUPER, 
      move_in, compositor);
  weston_compositor_add_key_binding(compositor, KEY_8, MODIFIER_SUPER, 
      move_out, compositor);
  weston_compositor_add_key_binding(compositor, KEY_9, MODIFIER_SUPER, 
      scale_up, compositor);
  weston_compositor_add_key_binding(compositor, KEY_0, MODIFIER_SUPER, 
      scale_down, compositor);

  /*// use this at some point in the future to detect and grab the rift display
  struct weston_output *output;
  wl_list_for_each(output, &compositor->output_list, link)
  {
    weston_log("Output (%i): %s\n\t%ix%i\n", output->id, output->name,
        output->width, output->height);
  }*/

  rift->distortion_shader = calloc(1, sizeof *(rift->distortion_shader));
  struct distortion_shader_ *d = rift->distortion_shader;
  d->program = CreateProgram(distortion_vertex_shader, distortion_fragment_shader);
  d->EyeToSourceUVScale = glGetUniformLocation(d->program, "EyeToSourceUVScale");
  d->EyeToSourceUVOffset = glGetUniformLocation(d->program, "EyeToSourceUVOffset");
  d->RightEye = glGetUniformLocation(d->program, "RightEye");
  d->angle = glGetUniformLocation(d->program, "angle");
  d->Position = glGetAttribLocation(d->program, "Position");
  d->TexCoord0 = glGetAttribLocation(d->program, "TexCoord0");
  d->TexCoordR = glGetAttribLocation(d->program, "TexCoordR");
  d->TexCoordG = glGetAttribLocation(d->program, "TexCoordG");
  d->TexCoordB = glGetAttribLocation(d->program, "TexCoordB");
  d->eyeTexture = glGetAttribLocation(d->program, "Texture0");

  rift->eye_shader = calloc(1, sizeof *(rift->eye_shader));
  struct eye_shader_ *e = rift->eye_shader;
  e->program = CreateProgram(eye_vertex_shader, eye_fragment_shader);
  e->Position = glGetAttribLocation(d->program, "Position");
  e->TexCoord0 = glGetAttribLocation(d->program, "TexCoord0");
  e->Projection = glGetUniformLocation(e->program, "Projection");
  e->ModelView = glGetUniformLocation(e->program, "ModelView");
  e->virtualScreenTexture = glGetAttribLocation(d->program, "Texture0");

  rift->scene = calloc(1, sizeof *(rift->scene));
  glGenBuffers(1, &rift->scene->vertexBuffer);
  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->vertexBuffer);
  static const GLfloat rectangle[] = 
    {-1.0f, -1.0f, -0.5f, 
      1.0f, -1.0f, -0.5f, 
     -1.0f, 1.0f, -0.5f,
      1.0f, -1.0f, -0.5f, 
      1.0f, 1.0f, -0.5f,
     -1.0f, 1.0f, -0.5f};
  glBufferData(GL_ARRAY_BUFFER, sizeof(rectangle), rectangle, GL_STATIC_DRAW);

  glGenBuffers(2, &rift->scene->SBSuvsBuffer[0]);
  glGenBuffers(1, &rift->scene->uvsBuffer);
  static const GLfloat uvs[3][12] = 
   {{ 0.0, 0.0,
      0.5, 0.0,
      0.0, 1.0,
      0.5, 0.0,
      0.5, 1.0,
      0.0, 1.0},
   {  0.5, 0.0,
      1.0, 0.0,
      0.5, 1.0,
      1.0, 0.0,
      1.0, 1.0,
      0.5, 1.0},
   {  0.0, 0.0,
      1.0, 0.0,
      0.0, 1.0,
      1.0, 0.0,
      1.0, 1.0,
      0.0, 1.0}};
  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->SBSuvsBuffer[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[0]), uvs[0], GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->SBSuvsBuffer[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[1]), uvs[1], GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, rift->scene->uvsBuffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(uvs[2]), uvs[2], GL_STATIC_DRAW);

  rift->width = 1920;
  rift->height = 1080;

  glGenTextures(1, &rift->fbTexture);
  glBindTexture(GL_TEXTURE_2D, rift->fbTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rift->width, rift->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &rift->redirectedFramebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, rift->redirectedFramebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rift->fbTexture, 0); show_error();
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    switch(glCheckFramebufferStatus(GL_FRAMEBUFFER))
    {
      case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: weston_log("incomplete attachment\n"); break;
      case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: weston_log("incomplete dimensions\n"); break;
      case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: weston_log("incomplete missing attachment\n"); break;
      case GL_FRAMEBUFFER_UNSUPPORTED: weston_log("unsupported\n"); break;
    }

    weston_log("framebuffer not working\n");
    show_error();
    exit(1);
  }
  glClear(GL_COLOR_BUFFER_BIT);

  /*EGLint pbufferAttributes[] = {
     EGL_WIDTH,           rift->width,
     EGL_HEIGHT,          rift->height,
     EGL_TEXTURE_FORMAT,  EGL_TEXTURE_RGB,
     EGL_TEXTURE_TARGET,  EGL_TEXTURE_2D,
     EGL_NONE
  };

  rift->pbuffer = eglCreatePbufferSurface(
      rift->egl_display, rift->egl_config, 
      pbufferAttributes);

  glGenTextures(1, &(rift->texture));
  glBindTexture(GL_TEXTURE_2D, rift->texture);
  //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rift->width, rift->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  eglMakeCurrent(rift->egl_display, rift->pbuffer, rift->pbuffer, rift->egl_context);
  eglBindTexImage(rift->egl_display, rift->pbuffer, EGL_BACK_BUFFER);
  eglMakeCurrent(rift->egl_display, rift->orig_surface, rift->orig_surface, rift->egl_context);*/

  ovr_Initialize();
  rift->hmd = ovrHmd_Create(0);
  if(rift->hmd == NULL)
  {
    rift->hmd = ovrHmd_CreateDebug(ovrHmd_DK2);
  }
  ovrHmd_ConfigureTracking(rift->hmd, ovrTrackingCap_Orientation | 
      ovrTrackingCap_Position | ovrTrackingCap_MagYawCorrection, 0);
  ovrHmd_ResetFrameTiming(rift->hmd, 0);

  int eye;
  for(eye = 0; eye < 2; eye++)
  {
    ovrFovPort fov = rift->hmd->DefaultEyeFov[eye];
    ovrEyeRenderDesc renderDesc = ovrHmd_GetRenderDesc(rift->hmd, eye, fov);
    struct EyeArg *eyeArg = &rift->eyeArgs[eye];

    eyeArg->projection = ovrMatrix4f_Projection(fov, 0.1, 100000, true);
    /*int j, k;
    for(k=0; k<4; k++)
    {
      for(j=0; j<4; j++)
      {
        printf("%f\t", eyeArg->projection.M[k][j]);
      }
      printf("\n");
    }*/
    rift->hmdToEyeOffsets[eye] = renderDesc.HmdToEyeViewOffset;
    ovrRecti texRect;
    texRect.Size = ovrHmd_GetFovTextureSize(rift->hmd, eye, rift->hmd->DefaultEyeFov[eye],
        1.0f);
    texRect.Pos.x = texRect.Pos.y = 0;
    eyeArg->textureWidth = texRect.Size.w;
    eyeArg->textureHeight = texRect.Size.h;

    glGenTextures(1, &eyeArg->texture);
    glBindTexture(GL_TEXTURE_2D, eyeArg->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, eyeArg->textureWidth, eyeArg->textureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &eyeArg->framebuffer); show_error();
    glBindFramebuffer(GL_FRAMEBUFFER, eyeArg->framebuffer); show_error();
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eyeArg->texture, 0); show_error();
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
      switch(glCheckFramebufferStatus(GL_FRAMEBUFFER))
      {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: weston_log("incomplete attachment\n"); break;
        case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: weston_log("incomplete dimensions\n"); break;
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: weston_log("incomplete missing attachment\n"); break;
        case GL_FRAMEBUFFER_UNSUPPORTED: weston_log("unsupported\n"); break;
      }

      weston_log("framebuffer not working\n");
      show_error();
      exit(1);
    }
    if(eye)
    {
      glClearColor(1.0, 0.0, 0.0, 1.0); show_error();
    }
    else
    {
      glClearColor(0.0, 1.0, 0.0, 1.0); show_error();
    }
    glClear(GL_COLOR_BUFFER_BIT); show_error();

    /*EGLint eyePbufferAttributes[] = {
       EGL_WIDTH,           texRect.Size.w,
       EGL_HEIGHT,          texRect.Size.h,
       EGL_TEXTURE_FORMAT,  EGL_TEXTURE_RGB,
       EGL_TEXTURE_TARGET,  EGL_TEXTURE_2D,
       EGL_NONE
    };

    eyeArg.surface = eglCreatePbufferSurface(
        rift->egl_display, rift->egl_config, 
        eyePbufferAttributes);*/

    ovrVector2f scaleAndOffset[2];
    ovrHmd_GetRenderScaleAndOffset(fov, texRect.Size, texRect, scaleAndOffset);
    eyeArg->scale = scaleAndOffset[0];
    eyeArg->offset = scaleAndOffset[1];

    ovrHmd_CreateDistortionMesh(rift->hmd, eye, fov, 0, &eyeArg->mesh);

    glGenBuffers(1, &eyeArg->indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eyeArg->indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, eyeArg->mesh.IndexCount * sizeof(unsigned short), eyeArg->mesh.pIndexData, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    float vertices_buffer[eyeArg->mesh.VertexCount*2];
    float uvs_buffer[3][eyeArg->mesh.VertexCount*2];
    uint i;
    for(i=0; i<eyeArg->mesh.VertexCount; i++)
    {
      ovrDistortionVertex vertex = eyeArg->mesh.pVertexData[i];
      vertices_buffer[i*2] = vertex.ScreenPosNDC.x;
      vertices_buffer[(i*2)+1] = vertex.ScreenPosNDC.y;
      uvs_buffer[0][i*2] = vertex.TanEyeAnglesR.x;
      uvs_buffer[0][(i*2)+1] = vertex.TanEyeAnglesR.y;
      uvs_buffer[1][i*2] = vertex.TanEyeAnglesG.x;
      uvs_buffer[1][(i*2)+1] = vertex.TanEyeAnglesG.y;
      uvs_buffer[2][i*2] = vertex.TanEyeAnglesB.x;
      uvs_buffer[2][(i*2)+1] = vertex.TanEyeAnglesB.y;
    }

    glGenBuffers(1, &eyeArg->vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg->vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, eyeArg->mesh.VertexCount * sizeof(GL_FLOAT) * 2, vertices_buffer, GL_STATIC_DRAW);
    glGenBuffers(3, &eyeArg->uvsBuffer[0]);
    for(i=0; i<3; i++)
    {
      glBindBuffer(GL_ARRAY_BUFFER, eyeArg->uvsBuffer[i]);
      glBufferData(GL_ARRAY_BUFFER, eyeArg->mesh.VertexCount * sizeof(GL_FLOAT) * 2, uvs_buffer[i], GL_STATIC_DRAW);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
  }

  return 0;
}
int
render_rift(struct weston_compositor *compositor, GLuint original_program)
{
  struct oculus_rift *rift = compositor->rift;

  // copy rift->pbuffer into rift->texture
  /*eglMakeCurrent(rift->egl_display, rift->pbuffer, rift->pbuffer, rift->egl_context);
  //glClearColor(0.5, 0.0, 0.5, 1.0);
  //glClear(GL_COLOR_BUFFER_BIT);
  glBindTexture(GL_TEXTURE_2D, rift->texture);
  eglReleaseTexImage(rift->egl_display, rift->pbuffer, EGL_BACK_BUFFER);
  eglBindTexImage(rift->egl_display, rift->pbuffer, EGL_BACK_BUFFER);
  eglMakeCurrent(rift->egl_display, rift->orig_surface, rift->orig_surface, rift->egl_context);*/
  // render eyes

  static int frameIndex = 0;
  ++frameIndex;
  ovrPosef eyePoses[2];
  ovrHmd_BeginFrameTiming(rift->hmd, frameIndex);
  ovrHmd_GetEyePoses(rift->hmd, frameIndex, rift->hmdToEyeOffsets, eyePoses, NULL);

  glEnable(GL_DEPTH_TEST);
  glUseProgram(rift->eye_shader->program);
  int i;
  for(i=0; i<2; i++)
  {
    const ovrEyeType eye = rift->hmd->EyeRenderOrder[i];
    struct EyeArg eyeArg = rift->eyeArgs[eye];
    
    ovrMatrix4f Model = initTranslationF(0.0, 0.0, rift->screen_z);
    Model = matrix4fMul(initScale(
          3.2 * rift->screen_scale, 
          1.8 * rift->screen_scale, 
          1.0), Model);
    ovrMatrix4f MV = matrix4fMul(posefToMatrix4f(eyePoses[eye]), Model);
    //MV = initIdentity();
    //MV.M[2][3] = 5;

    glBindFramebuffer(GL_FRAMEBUFFER, eyeArg.framebuffer);
    glViewport(0, 0, eyeArg.textureWidth, eyeArg.textureHeight);
    glClearColor(0.0, 0.0, 0.2, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUniform1i(rift->eye_shader->virtualScreenTexture, 0);
    glUniformMatrix4fv(rift->eye_shader->Projection, 1, GL_FALSE, &eyeArg.projection.M[0][0]);
    glUniformMatrix4fv(rift->eye_shader->ModelView, 1, GL_FALSE, &MV.M[0][0]);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glBindTexture(GL_TEXTURE_2D, rift->fbTexture);
    glBindBuffer(GL_ARRAY_BUFFER, rift->scene->vertexBuffer);
    glVertexAttribPointer(rift->eye_shader->Position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), NULL);
    if(rift->sbs == 1)
      glBindBuffer(GL_ARRAY_BUFFER, rift->scene->SBSuvsBuffer[eye]);
    else
      glBindBuffer(GL_ARRAY_BUFFER, rift->scene->uvsBuffer);
    glVertexAttribPointer(rift->eye_shader->TexCoord0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    //render_eye(rift, eyeArg);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // render distortion
  glUseProgram(rift->distortion_shader->program);
  glViewport(0, 0, 1920, 1080);

  glClearColor(0.0, 0.1, 0.0, 1.0);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);

  float angle = 0.0;
  if(rift->rotate == 1)
  {
    angle = 1.57079633; // 90 degrees, in radians
    glViewport(0, 0, 1080, 1920);
  }

  int eye;
  for(eye=0; eye<2; eye++)
  {
    struct EyeArg eyeArg = rift->eyeArgs[eye];
    glUniform2fv(rift->distortion_shader->EyeToSourceUVScale, 1, (float *)&eyeArg.scale);
    glUniform2fv(rift->distortion_shader->EyeToSourceUVOffset, 1, (float *)&eyeArg.offset);
    glUniform1i(rift->distortion_shader->RightEye, eye);
    glUniform1f(rift->distortion_shader->angle, angle);
    glUniform1i(rift->distortion_shader->eyeTexture, 0);

    //glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, eyeArg.texture);

    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.vertexBuffer);
    glVertexAttribPointer(rift->distortion_shader->Position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->Position);

    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[1]);
    glVertexAttribPointer(rift->distortion_shader->TexCoord0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoord0);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[0]);
    glVertexAttribPointer(rift->distortion_shader->TexCoordR, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoordR);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[1]);
    glVertexAttribPointer(rift->distortion_shader->TexCoordG, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoordG);
    glBindBuffer(GL_ARRAY_BUFFER, eyeArg.uvsBuffer[2]);
    glVertexAttribPointer(rift->distortion_shader->TexCoordB, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(rift->distortion_shader->TexCoordB);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eyeArg.indexBuffer);

    glDrawElements(GL_TRIANGLES, eyeArg.mesh.IndexCount, GL_UNSIGNED_SHORT, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  }

  //glEnable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);

  ovrHmd_EndFrameTiming(rift->hmd);

  // set program back to original shader program
  glUseProgram(original_program);
  return 0;
}
