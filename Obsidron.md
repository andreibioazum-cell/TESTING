# Obsidron (Obsidronium / Obsidian Core) Language Reference

> **Audience**: AI coding agents, compiler engineers, and game/application developers generating or maintaining `.obs` source files for **Obsidian Core** (**Obsidronium**).

---

## 1. Overview

**Obsidron** (also referred to as **Obsidronium**, file extension `.obs`, runtime `_VERSION == "Obsidronium 1.0 (Obsidian Core)"`) is a statically-transpilable, garbage-collected scripting and systems language implemented in C99 (`src/obsidian_core.c`, `src/obsidronium_runtime.h`).

Key characteristics:
- **Dual block syntax**: Supports both **indentation-based blocks** (`:` + indent/dedent, Python-style) and **`end`-delimited blocks** (`then ... end`, `do ... end`, `function ... end`).
- **Optional strict type annotations**: `int`, `float`, `str`, `bool` on variable declarations, function parameters, and `struct` / `class` fields.
- **Native data structures**: 1-indexed hybrid array/hash tables (`{...}`), dense `struct` / `class` definitions with single inheritance and `self` methods, first-class closures, and coroutines.
- **Multiple execution backends**:
  - **JIT / Bytecode VM** (`src/obsidian_jit.h`, `obsidian_project_open` / `obsidian_project_step` / `obsidian_project_exec` for embedded Studio apps on Android & desktop).
  - **C99 Transpiler / AOT** (`obsidian --emit-c`, `obsidian --aot`, `obsidian file.obs`).
- **Built-in Batteries**: 2D Canvas graphics, 3D Studio Scene SDK (`scripts/obsidian_sdk.obs`), 2D physics/entity engine (`engine`), procedural & WAV audio (`audio`), TCP/UDP sockets (`net`/`socket`), HTTP/HTTPS (`http`), WebSockets (`websocket`), JSON, crypto hashes, FFI (`ffi`), filesystem (`fs`), and path utilities (`path`).

---

## 2. Lexical Rules & Comments

### 2.1 Identifiers & Case Sensitivity
- Identifiers start with `[A-Za-z_]` followed by `[A-Za-z0-9_]`.
- Case-sensitive.

### 2.2 Comments
- **Single-line comment**: `-- comment until end of line`
- **Multi-line (long) comment**: `--[[ multi-line comment ]]` or `--[=[ ... ]=]` (any matching `=` count).

### 2.3 Literals
- **Nil**: `nil`
- **Booleans**: `true`, `false` (only `nil` and `false` are falsy; `0`, `0.0`, and `""` are truthy)
- **Numbers**:
  - Integer literals (`42`, `0xFF`) produce `int64_t` (`LC_INTEGER`) when no decimal point or exponent is present.
  - Floating literals (`3.14`, `1e-5`, `0x1.8p1`) produce `double` (`LC_NUMBER`).
- **Strings**:
  - Single quotes `'hello'`, double quotes `"hello"`, or long bracket strings `[[multi\nline]]`, `[=[raw]=]`.
  - Escape sequences: `\n`, `\r`, `\t`, `\a`, `\b`, `\f`, `\v`, `\\`, `\'`, `\"`, `\xHH`, `\DDD` (decimal byte).

### 2.4 Reserved Keywords
```text
nil      true     false    func     function  local    global
end      if       then     elseif   elif      else     while
do       for      in       repeat   until     return   break
goto     and      or       not      struct    class
```

Contextual type keywords (recognized before an identifier in declarations, parameters, and struct/class fields):
```text
int      float    str      bool
```

---

## 3. Variables, Scoping & Type Annotations

### 3.1 Scoping Rules
- **Implicit assignment** (`x = 10`): Assigns to an existing local/upvalue if found, otherwise creates/writes a **global** variable (in the top-level or module environment).
- **Explicit global** (`global x, y = 1, 2`): Forces global scope even inside nested blocks or functions.
- **Explicit local** (`local x = 10`): Creates a block-scoped local variable.
- **Typed declaration** (`int x = 10`, `float dt = 0.016`, `str name = "hero"`, `bool alive = true`): Automatically declares a **local** variable (or top-level variable) constrained to that type.
  - `local int x = 5` is also valid.
- **Compile-time constant** (`local pi <const> = 3.14159`): Verified during parsing/checking; reassigning raises a compile-time error.

### 3.2 Strict Runtime Types
| Annotation | Runtime Kind | Coercion / Validation Behavior |
|---|---|---|
| `int` | `LC_INTEGER` | Accepts integer or integral float; truncates/converts or errors on incompatible types. |
| `float` | `LC_NUMBER` | Accepts numeric values (`int` or `float`) stored as `double`. |
| `str` | `LC_STRING` | Requires string value. |
| `bool` | `LC_BOOL` | Coerces/validates boolean value. |
| *(omitted)* | Dynamic | Holds any `LcValue` (`nil`, `bool`, `number`, `integer`, `string`, `table`, `function`, `struct`, etc.). |

### 3.3 Multiple Assignment & Compound Operators
```obs
a, b = 10, 20
a, b = b, a

x += 5
x -= 2
x *= 3
x /= 2
x %= 4
```
Supported compound assignment operators: `+=`, `-=`, `*=`, `/=`, `%=`. (Target can be any assignable lvalue: variable, `t.field`, `t[idx]`, `self.hp`.)

---

## 4. Operators & Precedence

From lowest to highest precedence:
1. `or` (logical OR, short-circuiting, returns last evaluated operand)
2. `and` (logical AND, short-circuiting, returns last evaluated operand)
3. `<`, `<=`, `>`, `>=`, `==`, `~=` (relational; note `~=` is not-equal)
4. `|` (bitwise OR)
5. `~` (bitwise XOR)
6. `&` (bitwise AND)
7. `<<`, `>>` (bitwise left/right shift)
8. `..` (string concatenation, right-associative; coerces numbers)
9. `+`, `-` (addition, subtraction)
10. `*`, `/`, `//`, `%` (multiplication, float division, floor division, modulo)
11. Unary `not`, `-`, `#` (length of string or array part of table), `~` (bitwise NOT)
12. `^` (exponentiation, right-associative)

---

## 5. Control Flow

Both **colon + indentation** and **`then`/`do` ... `end`** forms are first-class and may be mixed across different statements (do not mix `:` and `end` in the same `if`/`while`/`for` header).

### 5.1 Conditionals
```obs
-- Indentation style (recommended)
if hp <= 0:
    alive = false
elif hp < 25:
    warn("Low health")
else:
    alive = true

-- Classic end-delimited style
if hp <= 0 then
    alive = false
elseif hp < 25 then
    warn("Low health")
else
    alive = true
end
```
Both `elif` and `elseif` are accepted in either style.

### 5.2 Loops
```obs
-- While loop
while running:
    step(0.016)

-- Numeric for loop: for var = start, stop [, step]
for i = 1, 10:
    print(i)

for i = 10, 1, -1:
    print(i)

-- Generic for loop
for k, v in pairs(tbl):
    print(k, v)

for i, item in ipairs(arr):
    print(i, item)

-- Repeat-until (always executes at least once)
repeat
    n -= 1
until n <= 0
```
- Use `break` to exit the innermost loop.
- Labels and jumps: `::label_name::` and `goto label_name`.

---

## 6. Functions & Closures

- Declared with `func` or `function`:
```obs
func add(int a, int b):
    return a + b

local func helper(x):
    return x * 2

-- Classic end-delimited
function mul(a, b)
    return a * b
end

-- Anonymous / lambda
cb = func(float dt):
    elapsed += dt
```
- **Method definition**: `func obj:method(arg): ...` automatically prepends an implicit `self` parameter.
- **Method call**: `obj:method(arg)` passes `obj` as the first argument (`self`).
- **Colon disambiguation**: Note that `if cond:` / `func f():` / `class Foo:` use `:` for blocks, whereas `obj:method(...)` uses `:` for method calls (followed by `(`, `{`, or string literal).
- **Varargs**: `func sum(...):` collects extra arguments (`...` and `select('#', ...)`).
- **Multiple return values**: `return a, b, c`. Parentheses `(f())` truncate to one value.

---

## 7. Tables

Tables (`{}`) are 1-indexed associative arrays/maps with optional metatables.
```obs
arr = {10, 20, 30}        -- #arr == 3, arr[1] == 10
vec = {x = 1.0, y = 2.0}  -- vec.x == 1.0, vec["y"] == 2.0
dyn = {[keyExpr] = val}
```
Metatables are supported via `setmetatable(t, mt)` and `getmetatable(t)`.

---

## 8. Structs & Classes

Obsidron supports native `struct` and `class` declarations with typed fields, default values, single inheritance, and bound `self` methods.

```obs
struct Vec2:
    float x = 0.0
    float y = 0.0

    func len():
        return math.sqrt(self.x * self.x + self.y * self.y)

class Entity:
    str name = "Entity"
    float hp = 100.0

    func damage(float amount):
        self.hp -= amount

class Player(Entity):
    int score = 0

    func add_score(int pts):
        self.score += pts
```

### Instantiation
Call the struct/class name like a constructor, optionally passing a table of initial field overrides:
```obs
p = Player({name = "Hero", score = 10})
p:damage(15.0)
print(p.name, p.hp, p.score)
```
- Empty body placeholder: `pass` is valid inside an otherwise empty `struct`/`class` body.
- Inherited classes copy/extend parent fields and methods; subclass methods override parent methods of the same name.

---

## 9. Modules (`require`)

```obs
snake_mod = require("scripts.snake")
json = require("json")
```
- Dots in module names map to directory separators (`scripts.snake` -> `scripts/snake.obs` or `scripts/snake.lua`).
- Built-in modules (`json`, `math`, `string`, `text`, `table`, `bit`, `bit32`, `utf8`, `io`, `os`, `fs`, `path`, `time`, `vec`, `net`, `socket`, `http`, `websocket`, `audio`, `engine`, `hex`, `base64`, `url`, `hash`, `ffi`, `coroutine`) are preloaded globally and in `package.loaded`.

---

## 10. Core Built-in Globals

| Function | Description |
|---|---|
| `print(...)` | Prints tab-separated `tostring()` representations followed by newline (captured in Studio Output console). |
| `warn(...)` | Emits warning text to stderr / Studio console. |
| `type(v)` | Returns `"nil"`, `"boolean"`, `"number"`, `"string"`, `"table"`, `"function"`, `"userdata"`, `"thread"`, `"struct"`. |
| `tostring(v)` / `tonumber(v [, base])` | Value conversion. |
| `assert(cond [, msg])` / `error(msg [, level])` | Assertions and error raising. |
| `pcall(fn, ...)` / `xpcall(fn, errh, ...)` | Protected calls returning `ok, res_or_err, ...`. |
| `select(index_or_hash, ...)` | Vararg inspection. |
| `pairs(t)` / `ipairs(t)` / `next(t [, k])` | Table iteration. |
| `rawget(t, k)` / `rawset(t, k, v)` / `rawequal(a, b)` / `rawlen(v)` | Bypass metatables. |
| `setmetatable(t, mt)` / `getmetatable(t)` | Metatable management. |
| `collectgarbage([opt [, arg]])` / `gcinfo()` | Incremental GC control. |
| `require(modname)` | Module loader. |
| `unpack(list [, i [, j]])` | Alias for `table.unpack`. |
| `_G`, `_VERSION` | Global table and `"Obsidronium 1.0 (Obsidian Core)"`. |

---

## 11. Built-in 2D Canvas & Project Lifecycle API

When running inside **Obsidian Core Studio** (2D project or HUD overlay on top of a 3D project), the runtime drives these optional global callbacks each frame:

```obs
func update(float dt):
    -- Called every frame with delta time in seconds
    pass

func draw():
    -- Called every frame to emit 2D draw commands (coordinates in screen pixels)
    pass

func on_touch(int id, float x, float y, bool down):
    -- Called on touch down / move / up events
    pass
```

### 2D Drawing & Screen Functions
All color components `r, g, b, a` are floats in `[0.0, 1.0]`. Coordinates are in screen pixels `(0, 0)` at top-left.

| Function | Signature |
|---|---|
| `screen_width()` | Returns current viewport width in pixels (`int`). |
| `screen_height()` | Returns current viewport height in pixels (`int`). |
| `clear_screen([r, g, b [, a]])` | Clears draw list and sets background color. |
| `scene_reset()` | Clears accumulated draw commands (`engine.reset_scene()`). |
| `draw_rect(x, y, w, h [, r, g, b, a [, rot_deg]])` | Draws filled rectangle. |
| `draw_rect_outline(x, y, w, h [, thickness, r, g, b, a [, rot_deg]])` | Draws rectangle outline. |
| `draw_round_rect(x, y, w, h [, radius, r, g, b, a [, rot_deg]])` | Draws rounded rectangle. |
| `draw_circle(cx, cy, radius [, r, g, b, a])` | Draws filled circle centered at `(cx, cy)`. |
| `draw_circle_outline(cx, cy, radius [, thickness, r, g, b, a])` | Draws circle outline. |
| `draw_line(x1, y1, x2, y2 [, thickness, r, g, b, a])` | Draws a line segment. |
| `set_font(name)` | Sets text font (`"default"`, `"bold"`, `"mono"`, or custom path). |
| `set_text_align(align)` | Sets horizontal alignment: `"left"`, `"center"`, `"right"`. |
| `draw_text(str, x, y [, size, r, g, b, a])` | Draws UTF-8 text. |
| `load_sprite(path)` | Loads PNG/JPEG sprite metadata and returns integer `sprite_id`. |
| `draw_sprite(sprite_id, x, y [, w, h, rot_deg])` | Draws a loaded sprite. |

---

## 12. 3D Engine SDK (`scripts/obsidian_sdk.obs`)

3D projects in Obsidian Core Studio combine a `scene.json` scene tree with `scripts/obsidian_sdk.obs` and `main.obs`.

### Standard 3D Entry Point (`main.obs`)
```obs
json = require("json")
sdk = require("scripts.obsidian_sdk")
scene = sdk.Scene()
world = scene:GetNode("World3D")

sdk.load_scene("scene.json", world)
require("scripts.game")

func update(float dt):
    scene:step(dt)
```

### Node Classes (`scene.json` & `sdk.Node.new`)
- **3D Geometry**: `Box`, `Wedge`, `Sphere`, `Cylinder`, `Plane`, `GltfModel`
- **Containers / Physics / Scene**: `Model`, `Folder`, `StaticBody3D`, `RigidBody3D`, `CollisionBox`, `CollisionSphere`, `Area3D`, `Camera3D`, `DirectionalLight3D`, `PointLight3D`, `Script`

### Node Properties (Exposed in Obsidronium Scripts)
- `node.Name` (`str`), `node.ClassName` (`str`)
- `node.Position` (`Vector3.new(x, y, z)` -> `.X`, `.Y`, `.Z`)
- `node.Size` (`Vector3.new(x, y, z)` -> `.X`, `.Y`, `.Z`)
- `node.Rotation` (`Vector3.new(rx, ry, rz)` in degrees)
- `node.Color` (`Color3.new(r, g, b)` in `[0, 1]` or `Color3.fromRGB(r, g, b)` in `[0, 255]`)
- `node.Material` (`"Plastic"`, `"SmoothPlastic"`, `"Metal"`, `"Neon"`, `"Wood"`, `"Glass"`, etc.)
- `node.Transparency` (`float` in `[0, 1]`)
- `node.Collidable` (`bool`), `node.Static` (`bool`)
- `node.AnimPath` (`str`), `node.AnimAutoPlay` (`bool`), `node.AnimLoop` (`bool`)

### Methods & Signals
- `parent:FindFirstChild(name)` / `parent:GetChildren()` / `node:Destroy()`
- `sdk.Node.new(className, parent)`
- `scene.OnUpdate:Connect(func(float dt): ...)`
- `sdk.load_animation(path)` / `sdk.play_animation(node, path [, loop])` / `sdk.stop_animation(node)`
  - Plays `.anim.json` keyframe clips (`{"name": "...", "loop": true, "keyframes": [{"t":0, "px":0, "py":2, "pz":0, "rx":0, "ry":0, "rz":0, "sx":2, "sy":2, "sz":2}]}`).
  - Imported `GltfModel` nodes (`.glb` / `.gltf`) support embedded skeletal/node animation clips edited directly in the Studio Animator (`modelPath`, `modelAnimIndex`, `animAutoPlay`, `animLoop`).

---

## 13. Standard Libraries Reference

### 13.1 `math`
- Constants: `math.pi`, `math.huge`, `math.maxinteger`, `math.mininteger`
- Functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `exp`, `log`, `pow`, `sqrt`, `floor`, `ceil`, `abs`, `deg`, `rad`, `min`, `max`, `fmod`, `modf`, `frexp`, `ldexp`, `random([m [, n]])`, `randomseed(x)`, `tointeger(x)`, `type(x)`, `ult(m, n)`

### 13.2 `string` & `text`
- `string`: `len`, `sub`, `upper`, `lower`, `rep`, `byte`, `char`, `reverse`, `format`, `find`, `match`, `gmatch`, `gsub`
- `text`: `trim`, `ltrim`, `rtrim`, `reverse`, `capitalize`, `title`, `swapcase`, `startswith(s, p)`, `endswith(s, p)`, `contains(s, p)`, `find(s, p [, at])`, `count(s, p [, at])`, `split(s, sep)`, `join(tbl [, sep])`, `replace(s, from, to [, limit])`, `lpad(s, w [, ch])`, `rpad(s, w [, ch])`, `center(s, w [, ch])`, `repeat(s, n)`

### 13.3 `table`
- `insert`, `remove`, `concat`, `pack`, `unpack`, `move`, `sort`

### 13.4 `bit` & `bit32`
- `bit`: `tobit`, `bnot`, `bswap`, `band`, `bor`, `bxor`, `lshift`, `rshift`, `arshift`, `rol`, `ror`, `tohex`
- `bit32`: `bnot`, `band`, `bor`, `bxor`, `btest`, `lshift`, `rshift`, `arshift`, `lrotate`, `rrotate`, `extract`, `replace`

### 13.5 `utf8`
- `char`, `len`, `codepoint`, `offset`, `codes`

### 13.6 `json`, `hex`, `base64`, `url`, `hash`
- `json.encode(val)` / `json.decode(str)` (returns `val` or `nil, err`)
- `hex.encode(s [, "upper"])` / `hex.decode(s)`
- `base64.encode(s)` / `base64.decode(s)`
- `url.encode(s)` / `url.decode(s)` / `url.queryencode(s)` / `url.querydecode(s)` / `url.parse(s)` / `url.build(tbl)`
- `hash`: `crc32`, `adler32`, `fnv1a`, `djb2`, `sha256`, `sha256raw`, `hmac_sha256(key, msg)`

### 13.7 `io`, `os`, `fs`, `path`, `time`
- `io`: `open`, `close`, `flush`, `input`, `output`, `lines`, `read`, `write`, `type`, `tmpfile`, `popen`
- `os`: `clock`, `time`, `date`, `difftime`, `getenv`, `remove`, `rename`, `tmpname`, `execute`, `exit`, `setlocale`
- `fs`: `stat`, `exists`, `isfile`, `isdir`, `size`, `modified`, `list`, `listdir`, `getcwd`, `mkdir`, `mkdirs`, `rmdir`, `chdir`, `remove`, `rename`, `readfile`, `writefile`, `appendfile`, `copyfile`, `touch`
- `path`: `basename`, `dirname`, `extension`, `ext`, `stem`, `join(...)`, `isabsolute`, `normalize`, `absolute`
- `time`: `now`, `monotonic`, `ticks`, `millis`, `sleep(sec)`, `iso([ts])`, `localiso([ts])`, `parts([ts])`, `localparts([ts])`

### 13.8 `vec` (2D Vector Helpers)
- `vec.add(x1, y1, x2, y2)`, `sub`, `mul`, `dot`, `cross`, `distance`, `distance2`, `lerp(x1, y1, x2, y2, t)`
- `vec.length(x, y)`, `length2`, `normalize`, `scale(x, y, s)`, `perpendicular`, `angle`, `rotate(x, y, rad)`, `clamp(x, y, maxLen)`, `reflect(x, y, nx, ny)`

### 13.9 `engine` (2D Entity & Physics World)
- World: `engine.world()`, `engine.update(w, dt)`, `engine.pause(w)`, `engine.resume(w)`, `engine.clear(w)`, `engine.set_gravity(w, gx, gy)`, `engine.camera(w [, x, y, zoom])`, `engine.world_to_screen(w, x, y)`, `engine.screen_to_world(w, x, y)`
- Entities: `engine.create(w [, name, x, y])` / `entity`, `find(w, id)`, `destroy(w, id)`, `list(w)`, `ids(w)`, `count(w)`, `set_position`, `set_velocity`, `set_size`, `translate`, `set_rotation`, `rotate`, `set_scale`, `set_angular_velocity`, `set_active`, `set_color`, `set_body(e, opts)`, `set_sprite(e, tex [, w, h])`, `load_texture(path)`
- Collisions & Tags: `intersects(a, b)`, `collisions(w, e)`, `bounds(e)`, `add_component`, `get_component`, `remove_component`, `add_tag`, `remove_tag`, `has_tag`
- Input & Overlay: `is_key_down`, `is_key_pressed`, `mouse_pos`, `is_mouse_down`, `is_mouse_clicked`, `touch_pos`, `inject_key`, `inject_mouse`, `inject_touch`, `clear_input`, `text`, `clear_text`

### 13.10 `audio` (PCM / WAV / Synthesis)
- Constructors: `audio.silence(sec [, rate, ch])`, `audio.tone(freq, sec [, opts])` (`waveform`: `"sine"|"square"|"saw"|"triangle"|"noise"`, `amplitude`, `attack`, `release`, `pan`, `sample_rate`, `channels`), `audio.load(wav_path)`, `audio.mix(...)`
- Buffer methods & functions: `save(path [, "float"])`, `info()`, `frames()`, `channels()`, `sample_rate()`, `duration()`, `get(frame, ch)`, `set(frame, ch, val)`, `gain(g)`, `fade(in_sec, out_sec)`, `normalize([peak])`, `lowpass(hz)`, `highpass(hz)`, `delay(sec, feedback, wet)`, `resample(rate)`, `play()`, `stop()`, `wait()`, `playback_available()`

### 13.11 `net` / `socket`, `http`, `websocket`
- `net` / `socket`: `tcp()`, `udp()`, `connect(host, port)`, `bind(host, port)`, `select(read_list, write_list, timeout)`, `resolve(host)`, `addresses(host)`, `gethostname()`
  - Socket methods: `connect`, `bind`, `listen`, `accept`, `send`, `receive`/`recv`, `sendto`, `receivefrom`/`recvfrom`, `close`, `settimeout`, `setoption`, `getsockname`, `getpeername`, `shutdown`
- `http`: `http.get(url)`, `http.post(url [, body, content_type])`, `http.request(url [, method, body, content_type])`, `http.tls_available()`, `http.setca(path)`, `http.setverify(bool)` -> returns `body, status, headers` (or `nil, err`)
- `websocket`: `websocket.connect("ws://..." | "wss://...")`, `websocket.listen(host, port [, opts])`
  - Methods: `accept()`, `send(data [, "text"|"binary"])`, `receive()` / `recv()`, `ping([data])`, `pong([data])`, `close([code])`, `settimeout(sec)`

### 13.12 `coroutine` & `ffi`
- `coroutine`: `create(fn)`, `resume(co, ...)`, `yield(...)`, `wrap(fn)`, `status(co)`, `running()`, `isyieldable()`
- `ffi` (runtime `libffi`): `lib = ffi.open(path)`, `ffi.call(lib, symbol, "ret(arg1,arg2,...)", ...)`, `ffi.close(lib)`
  - Supported FFI types: `void`, `bool`, `int8`, `uint8`, `int16`, `uint16`, `int32`/`int`, `uint32`/`uint`, `int64`, `uint64`, `float`, `double`, `pointer`/`ptr`, `string`, `size_t`, `ptrdiff_t`

---

## 14. CLI & Embedding C API

### CLI (`obsidian`)
```sh
obsidian script.obs                  # JIT compile and run
obsidian --check script.obs          # Syntax & const check only
obsidian --emit-c script.obs -o a.c  # Emit standalone C99 source
obsidian --aot script.obs -o app     # Ahead-of-time native binary
```

### C Embedding API (`src/obsidian_core.h`)
```c
char *obsidian_compile_c(const char *source, const char *filename, char **error_message);
int   obsidian_check(const char *source, const char *filename, char **error_message);

/* Embedded project runtime (compile with -DOBSIDIAN_EMBEDDED_RUNTIME) */
void *obsidian_project_open(const char *root_path, char **error_message);
char *obsidian_project_step(void *project, double dt);
char *obsidian_project_exec(void *project, const char *code);
void  obsidian_project_set_viewport(void *project, int width, int height);
void  obsidian_project_touch(void *project, int id, double x, double y, int down);
void  obsidian_project_close(void *project);
void  obsidian_free(void *memory);
```

---

## 15. Best Practices for AI Agents Writing Obsidron Code

1. **Prefer indentation-based blocks** (`func`, `if ...:`, `elif ...:`, `else:`, `for ...:`, `while ...:`, `struct Name:`, `class Name:`) to match all built-in Obsidian Core Studio templates.
2. **Use strict types (`int`, `float`, `str`, `bool`)** on numeric/hot-loop variables and class fields for clearer intent and better C/JIT code generation.
3. **Remember 1-based indexing** on tables (`for i = 1, #list:`), `~= ` for inequality, and `..` for string concatenation (`tostring(val)` when joining numbers).
4. **3D Studio physics**: When writing custom 3D controllers (`scripts/player.obs`), update `player.Position`, `player.Rotation`, and `cam.Position`/`cam.Rotation` inside `scene.OnUpdate:Connect(on_update)`. The Studio runtime calls `sdk.sync()` automatically after `update(dt)`.
5. **Never mix `then`/`do` with `:`** on the same block header.
