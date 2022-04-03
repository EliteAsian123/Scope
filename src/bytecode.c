#include "bytecode.h"

// TODO: Get rid of pointer types

// TODO: Proper accessors `.`
// TODO: Array indexers as accessors
// TODO: Utilities
// TODO: Types
// TODO: size_t for goto and stuff
// TODO: static checking

#define basicOperation(op)                                                                    \
	b = pop();                                                                                \
	a = pop();                                                                                \
	if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {                                     \
		push((StackElem){.type = type(TYPE_INT), .v.v_int = a.v.v_int op b.v.v_int});         \
	} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {                          \
		push((StackElem){.type = type(TYPE_FLOAT), .v.v_float = a.v.v_float op b.v.v_float}); \
	} else {                                                                                  \
		ierr("Invalid types for `" #op "`.");                                                 \
	}

#define boolOperation(op)                                                                  \
	b = pop();                                                                             \
	a = pop();                                                                             \
	if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {                                  \
		push((StackElem){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int op b.v.v_int});     \
	} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {                       \
		push((StackElem){.type = type(TYPE_BOOL), .v.v_int = a.v.v_float op b.v.v_float}); \
	} else {                                                                               \
		ierr("Invalid types for `" #op "`.");                                              \
	}

#define toStr(x, s)                           \
	int length = snprintf(NULL, 0, s, x) + 1; \
	char* str = malloc(length);               \
	snprintf(str, length, s, x);              \
	push((StackElem){.type = type(TYPE_STR), .v.v_string = cstrToStr(str)});

#define ierr(...)                                          \
	fprintf(stderr, "Interpret Error: " __VA_ARGS__ "\n"); \
	exit(-1);

#define iwarn(...) fprintf(stdout, "Interpret Warning: " __VA_ARGS__ "\n");

#define jump(loc) i = loc - 1;

#define curInstBuf instbuffer[instbufferCount - 1]

typedef struct {
	Object* vars;
	size_t varsCount;
} ObjectList;

typedef struct {
	ObjectList* o;
} CallFrame;

typedef struct {
	Inst* insts;
	size_t instsCount;
} InstBuffer;

// Interpret stage
static CallFrame frames[STACK_SIZE];
static size_t framesCount;

// Interpret stage
static StackElem stack[STACK_SIZE];
static size_t stackCount;

// Parse stage
static int locstack[STACK_SIZE];
static size_t locstackCount;

// Parse stage
static int loopstack[STACK_SIZE];
static size_t loopstackCount;

// Interpret stage
static char* argstack[STACK_SIZE];
static size_t argstackCount;

// Interpret stage
static FuncPointer* funcs;
static size_t funcsCount;

// Parse stage
static InstBuffer instbuffer[STACK_SIZE];
static size_t instbufferCount;
static bool isInBuffer;

void pushFrame(CallFrame frame) {
	frames[framesCount++] = frame;

	if (framesCount >= STACK_SIZE) {
		ierr("Call frame stack overflowed.");
	}
}

CallFrame popFrame() {
	return frames[--framesCount];
}

void push(StackElem elem) {
	stack[stackCount++] = elem;

	if (stackCount >= STACK_SIZE) {
		ierr("Internal stack size exceeded.");
	}
}

StackElem pop() {
	return stack[--stackCount];
}

void pushLoc() {
	locstack[locstackCount++] = instsCount;

	if (locstackCount >= STACK_SIZE) {
		ierr("Location stack size exceeded.");
	}
}

void pushSLoc(int loc) {
	locstack[locstackCount++] = loc;
}

int popLoc() {
	return locstack[--locstackCount];
}

int readLoc() {
	return locstack[locstackCount - 1];
}

void pushLoop() {
	loopstack[loopstackCount++] = instsCount;

	if (loopstackCount >= STACK_SIZE) {
		ierr("Loop stack size exceeded.");
	}
}

int popLoop() {
	return loopstack[--loopstackCount];
}

int readLoop() {
	return loopstack[loopstackCount - 1];
}

void pushArg(char* arg) {
	argstack[argstackCount++] = arg;

	if (argstackCount >= STACK_SIZE) {
		ierr("Location stack size exceeded.");
	}
}

char* popArg() {
	return argstack[--argstackCount];
}

static int pushFunc(int loc, TypeInfo type) {
	FuncPointer f;
	f.location = loc;
	f.type = type;
	f.argsLen = type.argsLen - 1;
	f.args = malloc(sizeof(char*) * f.argsLen);
	for (int i = f.argsLen - 1; i >= 0; i--) {
		f.args[i] = popArg();
	}

	funcsCount++;
	funcs = realloc(funcs, sizeof(FuncPointer) * funcsCount);
	funcs[funcsCount - 1] = f;

	return funcsCount - 1;
}

static void delVarAtIndex(ObjectList* o, size_t i) {
	// TODO: Free
	free(o->vars[i].name);
	free(o->vars[i].ptr);
	freeTypeInfo(o->vars[i].type);

	// Ripple down
	for (int j = i; j < o->varsCount - 1; j++) {
		o->vars[j] = o->vars[j + 1];
	}

	// Resize
	o->varsCount--;
	o->vars = (Object*) realloc(o->vars, sizeof(Object) * o->varsCount);
}

static void delVar(ObjectList* o, const char* n) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (strcmp(o->vars[i].name, n) == 0) {
			delVarAtIndex(o, i);
		}
	}
}

static void setVar(ObjectList* o, Object obj) {
	delVar(o, obj.name);

	o->varsCount++;
	o->vars = realloc(o->vars, sizeof(Object) * o->varsCount);
	o->vars[o->varsCount - 1] = obj;
}

static void dupVar(ObjectList* o, Object obj) {
	Object d = obj;
	d.name = strdup(obj.name);
	d.type = dupTypeInfo(obj.type);
	d.ptr = typedup(d.type.id, obj.ptr);

	setVar(o, d);
}

static Object getVar(ObjectList* o, const char* n) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (strcmp(o->vars[i].name, n) == 0) {
			return o->vars[i];
		}
	}

	printf("Unknown variable `%s`.\n", n);
	ierr("Use of undeclared variable.");
}

static bool isVar(ObjectList* o, const char* n) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (strcmp(o->vars[i].name, n) == 0) {
			return true;
		}
	}

	return false;
}

static void disposeVar(ObjectList* o) {
	for (size_t i = 0; i < o->varsCount; i++) {
		if (o->vars[i].ptr != NULL) {
			free(o->vars[i].name);
			free(o->vars[i].ptr);
			freeTypeInfo(o->vars[i].type);
		}
	}
	free(o->vars);
}

void pushInst(Inst i, int scope) {
	i.scope = scope;
	i.location = instsCount;

	if (!isInBuffer) {
		instsCount++;
		insts = realloc(insts, sizeof(Inst) * instsCount);
		insts[instsCount - 1] = i;
	} else {
		curInstBuf.instsCount++;
		curInstBuf.insts = realloc(curInstBuf.insts, sizeof(Inst) * curInstBuf.instsCount);
		curInstBuf.insts[curInstBuf.instsCount - 1] = i;
	}
}

void setInst(Inst i, int loc, int scope) {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: `setInst` cannot be used in a move buffer.\n");
		exit(-1);
	}

	i.scope = scope;
	i.location = loc;

	insts[loc] = i;
}

void pushInstAt(Inst i, int loc, int scope) {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: `pushInstAt` cannot be used in a move buffer.\n");
		exit(-1);
	}

	i.scope = scope;
	i.location = loc;

	instsCount++;
	insts = realloc(insts, sizeof(Inst) * instsCount);

	for (size_t i = instsCount - 1; i > loc; i--) {
		insts[i] = insts[i - 1];
	}
	insts[loc] = i;
}

void startMoveBuffer() {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: We are already in a move buffer.\n");
		exit(-1);
	}

	instbufferCount++;
	if (instbufferCount >= STACK_SIZE) {
		fprintf(stderr, "Parse Error: Move buffer stack size exceeded.\n");
	}

	curInstBuf.insts = malloc(0);
	curInstBuf.instsCount = 0;
	isInBuffer = true;
}

void endMoveBuffer() {
	if (!isInBuffer) {
		fprintf(stderr, "Parse Error: We are not in a move buffer.\n");
		exit(-1);
	}

	isInBuffer = false;
}

void putMoveBuffer(int scope) {
	if (isInBuffer) {
		fprintf(stderr, "Parse Error: Cannot `putMoveBuffer` when we are in one!\n");
		exit(-1);
	}

	if (instbufferCount <= 0) {
		fprintf(stderr, "Parse Error: Move buffer stack is empty!\n");
		exit(-1);
	}

	for (size_t i = 0; i < curInstBuf.instsCount; i++) {
		pushInst(curInstBuf.insts[i], scope);
	}

	free(curInstBuf.insts);
	instbufferCount--;
}

static String cstrToStr(const char* ptr) {
	String str = (String){
		.len = strlen(ptr),
	};
	memcpy(str.chars, ptr, str.len);

	return str;
}

static bool stringEqual(String a, String b) {
	if (a.len != b.len) {
		return false;
	}

	for (size_t i = 0; i < a.len; i++) {
		if (a.chars[i] != b.chars[i]) {
			return false;
		}
	}

	return true;
}

void bc_init() {
	insts = malloc(0);
	instsCount = 0;

	stackCount = 0;

	funcs = malloc(0);
	funcsCount = 0;
}

static void instDump(size_t i) {
	const char* instName;
	bool isStringArg = false;

	// clang-format off
	static_assert(_INSTS_ENUM_LEN == 34, "Update bytecode strings.");
	switch (insts[i].inst) {
		case LOAD: 		instName = "load"; 							break;
		case LOADT: 	instName = "loadt"; 						break;
		case LOADV: 	instName = "loadv"; 	isStringArg = true; break;
		case LOADA: 	instName = "loada"; 	isStringArg = true; break;
		case SAVEV: 	instName = "savev"; 	isStringArg = true; break;
		case RESAVEV: 	instName = "resavev"; 	isStringArg = true; break;
		case SAVEF: 	instName = "savef"; 						break;
		case CALLF: 	instName = "callf"; 	isStringArg = true;	break;
		case ENDF: 		instName = "endf"; 							break;
		case EXTERN: 	instName = "extern"; 						break;
		case APPENDT: 	instName = "appendt"; 						break;
		case ARRAYI: 	instName = "arrayi"; 						break;
		case ARRAYS: 	instName = "arrays"; 	isStringArg = true; break;
		case ARRAYG: 	instName = "arrayg"; 	isStringArg = true; break;
		case ARRAYL: 	instName = "arrayl"; 						break;
		case SWAP: 		instName = "swap"; 							break;
		case NOT: 		instName = "not"; 							break;
		case AND: 		instName = "and"; 							break;
		case OR: 		instName = "or"; 							break;
		case ADD: 		instName = "add"; 							break;
		case SUB: 		instName = "sub"; 							break;
		case MUL: 		instName = "mul"; 							break;
		case DIV: 		instName = "div"; 							break;
		case MOD: 		instName = "mod"; 							break;
		case NEG: 		instName = "neg"; 							break;
		case EQ: 		instName = "eq"; 							break;
		case GT: 		instName = "gt"; 							break;
		case LT: 		instName = "lt"; 							break;
		case GTE: 		instName = "gte"; 							break;
		case LTE: 		instName = "lte"; 							break;
		case CSTR: 		instName = "cstr"; 							break;
		case GOTO: 		instName = "goto"; 							break;
		case IFN:	 	instName = "ifn"; 							break;
		default: 		instName = "?"; 							break;
	}
	// clang-format on

	if (insts[i].inst == LOAD && insts[i].type.id == TYPE_STR) {
		printf("[%d, %d] %s: %s, %s\n", i, insts[i].scope, instName,
			   insts[i].a.v_string.chars, typestr(insts[i].type.id));
	} else if (isStringArg) {
		printf("[%d, %d] %s: %s, %s\n", i, insts[i].scope, instName,
			   insts[i].a.v_string.chars, typestr(insts[i].type.id));
	} else {
		printf("[%d, %d] %s: %d, %s\n", i, insts[i].scope, instName,
			   insts[i].a.v_int, typestr(insts[i].type.id));
	}
}

static void bc_dump() {
	for (size_t i = 0; i < instsCount; i++) {
		instDump(i);
	}
}

static void readByteCode(size_t frameIndex, size_t start, bool showCount) {
	int funcScope = -1;
	int lastKnownScope = 0;
	CallFrame frame = frames[frameIndex];

	for (size_t i = start; i < instsCount; i++) {
		if (showCount) {
			instDump(i);
		}

		// Adjust scope

		int curScope = insts[i].scope;
		if (funcScope > -1) {
			curScope += funcScope;
		}

		if (curScope < lastKnownScope) {
			lastKnownScope = curScope;

			// Delete all variables that are now outside of scope
			for (size_t v = 0; v < frame.o->varsCount; v++) {
				if (frame.o->vars[v].scope > lastKnownScope) {
					delVarAtIndex(frame.o, v);
					v--;
				}
			}
		} else {
			lastKnownScope = curScope;
		}

		Object obj;
		Array arr;
		int s;

		StackElem a, b, c;
		a.type = type(TYPE_VOID);
		b.type = type(TYPE_VOID);
		c.type = type(TYPE_VOID);

		static_assert(_INSTS_ENUM_LEN == 34, "Update bytecode interpreting.");
		switch (insts[i].inst) {
			case LOAD:
				push((StackElem){.type = insts[i].type, .v = insts[i].a});
				break;
			case LOADT:
				push((StackElem){.type = dupTypeInfo(insts[i].type)});
				break;
			case LOADV:
				obj = getVar(frame.o, insts[i].a.v_ptr);

				if (obj.fromArgs && obj.type.id == TYPE_FUNC) {
					ierr("Functions from arguments can only be called.");
				}

				push(ptrToStackElem(obj.type, obj.ptr));
				break;
			case LOADA:
				pushArg(strdup(insts[i].a.v_ptr));
				break;
			case SAVEV:
				b = pop();
				a = pop();

				if (a.type.id != TYPE_UNKNOWN) {
					if (!typeInfoEqual(b.type, a.type)) {
						ierr("Declaration type doesn't match expression.");
					}
				}

				obj = (Object){
					.name = strdup(insts[i].a.v_ptr),
					.type = dupTypeInfo(b.type),
					.ptr = stackElemToPtr(b),
					.scope = curScope,
				};
				setVar(frame.o, obj);
				break;
			case RESAVEV:
				a = pop();

				obj = getVar(frame.o, insts[i].a.v_ptr);
				if (obj.fromArgs) {
					ierr("Cannot assign to an argument.");
				}

				if (!typeInfoEqual(a.type, obj.type)) {
					ierr("Assignment type doesn't match expression.");
				}

				obj = (Object){
					.name = strdup(insts[i].a.v_ptr),
					.type = dupTypeInfo(a.type),
					.ptr = stackElemToPtr(a),
					.scope = obj.scope,
				};
				setVar(frame.o, obj);
				break;
			case SAVEF:
				a = pop();

				push((StackElem){.type = dupTypeInfo(a.type), .v.v_int = pushFunc(i + 1, a.type)});
				jump(insts[i].a.v_int);

				break;
			case CALLF:
				obj = getVar(frame.o, (char*) insts[i].a.v_ptr);
				if (insts[i].type.id == TYPE_UNKNOWN &&
					obj.type.args[0].id == TYPE_VOID) {
					ierr("Invoke expression cannot be referencing a void function.");
				} else if (insts[i].type.id == TYPE_VOID &&
						   obj.type.args[0].id != TYPE_VOID) {
					ierr("Invoke statement cannot be referencing a non-void function.");
				}

				FuncPointer f = funcs[*(int*) obj.ptr];
				s = insts[f.location].scope;

				ObjectList fo;
				fo.vars = malloc(0);
				fo.varsCount = 0;

				if (obj.fromArgs) {
					popFrame();
				}
				CallFrame funcFrame = frames[framesCount - 1];

				// Duping may not even be required?
				// TODO: Optimize
				for (size_t v = 0; v < funcFrame.o->varsCount; v++) {
					Object vobj = funcFrame.o->vars[v];
					if (vobj.scope < s) {
						dupVar(&fo, vobj);
					}
				}

				for (int v = f.argsLen - 1; v >= 0; v--) {
					StackElem e = pop();

					if (!typeInfoEqual(f.type.args[v + 1], e.type)) {
						ierr("Type mismatch in function arguments.");
					}

					Object vo = (Object){
						.name = strdup(f.args[v]),
						.type = dupTypeInfo(e.type),
						.ptr = stackElemToPtr(e),
						.scope = s,
						.fromArgs = true,
					};
					setVar(&fo, vo);
				}

				pushFrame((CallFrame){.o = &fo});
				readByteCode(framesCount - 1, f.location, showCount);
				popFrame();

				// TODO: Optimize
				for (size_t v = 0; v < fo.varsCount; v++) {
					Object vobj = fo.vars[v];

					// Skip if it is a local variable
					if (vobj.scope >= s) {
						continue;
					}

					// Skip if the current frame doesn't contain the variable
					if (!isVar(funcFrame.o, vobj.name)) {
						continue;
					}

					// Skip if the variable is an argument
					if (obj.fromArgs) {
						continue;
					}

					// Get the var in the frame
					Object oobj = getVar(funcFrame.o, vobj.name);

					// Skip if the scopes don't match
					if (vobj.scope != oobj.scope) {
						continue;
					}

					// Dup to the frame
					dupVar(funcFrame.o, vobj);
				}

				disposeVar(&fo);

				if (obj.fromArgs) {
					pushFrame(frame);
				}

				break;
			case ENDF:
				// RETURN not break
				return;
			case EXTERN:
				a = pop();
				externs[a.v.v_int]();

				break;
			case APPENDT:
				b = pop();
				a = pop();

				a.type.argsLen++;
				if (a.type.argsLen == 0 && a.type.args == NULL) {
					a.type.args = malloc(sizeof(TypeInfo));
					a.type.args[0] = dupTypeInfo(b.type);
				} else {
					a.type.args = realloc(a.type.args, sizeof(TypeInfo) * a.type.argsLen);
					a.type.args[a.type.argsLen - 1] = dupTypeInfo(b.type);
				}

				push(a);

				break;
			case ARRAYI:  // `new a[b]`
				b = pop();
				a = pop();

				if (b.type.id != TYPE_INT || b.v.v_int < 0) {
					ierr("Expected positive int in array initilization.");
				}

				// Initilize the new stack element
				c = (StackElem){
					.type = type(TYPE_ARRAY),
				};

				// Convert type into array type
				c.type.argsLen = 1;
				c.type.args = malloc(sizeof(TypeInfo));
				c.type.args[0] = dupTypeInfo(a.type);

				// Initilize the array
				c.v.v_array.arr = malloc(sizeof(ValueHolder) * b.v.v_int);
				c.v.v_array.len = b.v.v_int;

				// Populate array
				for (int i = 0; i < b.v.v_int; i++) {
					c.v.v_array.arr[i] = createDefaultType(a.type);
				}

				// Push
				push(c);

				break;
			case ARRAYS:  // `$[a] = b`
				b = pop();
				a = pop();

				if (a.type.id != TYPE_INT) {
					ierr("Index must be an int.");
				}

				if (a.v.v_int < 0) {
					ierr("Index must be more than 0.");
				}

				obj = getVar(frame.o, insts[i].a.v_ptr);

				if (obj.type.id != TYPE_ARRAY) {
					ierr("The object referenced is not an array.");
				}

				arr = *(Array*) obj.ptr;

				if (a.v.v_int >= arr.len) {
					ierr("Index must be less than the length.");
				}

				if (!typeInfoEqual(b.type, obj.type.args[0])) {
					ierr("Set type doens't match expression.");
				}

				arr.arr[a.v.v_int] = b.v;

				break;
			case ARRAYG:  // `$[a]`
				a = pop();

				if (a.type.id != TYPE_INT) {
					ierr("Index must be an int.");
				}

				if (a.v.v_int < 0) {
					ierr("Index must be more than 0.");
				}

				obj = getVar(frame.o, insts[i].a.v_ptr);

				if (obj.type.id != TYPE_ARRAY) {
					ierr("The object referenced is not an array.");
				}

				arr = *(Array*) obj.ptr;

				if (a.v.v_int >= arr.len) {
					ierr("Index must be less than the length.");
				}

				push((StackElem){
					.type = dupTypeInfo(obj.type.args[0]),
					.v = arr.arr[a.v.v_int],
				});

				break;
			case ARRAYL:
				obj = getVar(frame.o, insts[i].a.v_ptr);

				if (obj.type.id != TYPE_ARRAY) {
					ierr("The object referenced is not an array.");
				}

				arr = *(Array*) obj.ptr;

				push((StackElem){
					.type = type(TYPE_INT),
					.v.v_int = arr.len,
				});

				break;
			case SWAP:
				b = stack[stackCount - 1];
				a = stack[stackCount - 2];

				stack[stackCount - 2] = b;
				stack[stackCount - 1] = a;

				break;
			case NOT:
				a = pop();

				if (a.type.id == TYPE_BOOL) {
					push((StackElem){.type = type(TYPE_BOOL), .v.v_int = !a.v.v_int});
				} else {
					ierr("Invalid type for `not`.");
				}

				break;
			case AND:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_BOOL && b.type.id == TYPE_BOOL) {
					push((StackElem){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int && b.v.v_int});
				} else {
					ierr("Invalid types for `and`.");
				}

				break;
			case OR:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_BOOL && b.type.id == TYPE_BOOL) {
					push((StackElem){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int || b.v.v_int});
				} else {
					ierr("Invalid types for `and`.");
				}

				break;
			case ADD:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {
					push((StackElem){.type = type(TYPE_INT), .v.v_int = a.v.v_int + b.v.v_int});
				} else if (a.type.id == TYPE_FLOAT && b.type.id == TYPE_FLOAT) {
					push((StackElem){.type = type(TYPE_FLOAT), .v.v_float = a.v.v_float + b.v.v_float});
				} else if (a.type.id == TYPE_STR && b.type.id == TYPE_STR) {
					// Create string
					String s = (String){
						.len = a.v.v_string.len + b.v.v_string.len,
					};
					s.chars = malloc(s.len);

					// Combine the two strings
					memcpy(s.chars, a.v.v_string.chars, a.v.v_string.len);
					memcpy(s.chars + a.v.v_string.len - 1, b.v.v_string.chars, b.v.v_string.len);

					// Push onto stack
					push((StackElem){.type = type(TYPE_STR), .v.v_string = s});
				} else {
					ierr("Invalid types for `add`.");
				}

				break;
			case SUB:
				basicOperation(-);
				break;
			case MUL:
				basicOperation(*);
				break;
			case DIV:
				basicOperation(/);
				break;
			case MOD:
				b = pop();
				a = pop();
				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT) {
					push((StackElem){.type = type(TYPE_INT), .v.v_int = a.v.v_int % b.v.v_int});
				} else {
					ierr("Invalid types for `%`.");
				}

				break;
			case NEG:
				a = pop();
				push((StackElem){.type = type(TYPE_INT), .v.v_int = -a.v.v_int});

				break;
			case EQ:
				b = pop();
				a = pop();

				if (a.type.id == TYPE_INT && b.type.id == TYPE_INT ||
					a.type.id == TYPE_FUNC && b.type.id == TYPE_FUNC) {
					push((StackElem){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int == b.v.v_int});
				} else if (a.type.id == TYPE_BOOL && b.type.id == TYPE_BOOL) {
					iwarn("Operation can be simplified for `eq`.");
					push((StackElem){.type = type(TYPE_BOOL), .v.v_int = a.v.v_int == b.v.v_int});
				} else if (a.type.id == TYPE_STR && a.type.id == TYPE_STR) {
					push((StackElem){
						.type = type(TYPE_BOOL),
						.v.v_int = stringEqual(a.v.v_string, b.v.v_string),
					});
				} else {
					ierr("Invalid types for `eq`.");
				}

				break;
			case GT:
				boolOperation(>);
				break;
			case LT:
				boolOperation(<);
				break;
			case GTE:
				boolOperation(>=);
				break;
			case LTE:
				boolOperation(<=);
				break;
			case CSTR:
				a = pop();

				if (a.type.id == TYPE_INT) {
					toStr(a.v.v_int, "%d");
				} else if (a.type.id == TYPE_FLOAT) {
					toStr(a.v.v_float, "%.9g");
				} else if (a.type.id == TYPE_BOOL) {
					if (a.v.v_int) {
						push((StackElem){.type = type(TYPE_STR), .v.v_string = cstrToStr("true")});
					} else {
						push((StackElem){.type = type(TYPE_STR), .v.v_string = cstrToStr("false")});
					}
				} else {
					printf("Cannot cast type %d.\n", a.type.id);
					ierr("Invalid type for `cstr`.");
				}

				break;
			case GOTO:
				jump(insts[i].a.v_int);
				break;
			case IFN:
				a = pop();

				if (a.type.id != TYPE_BOOL) {
					ierr("Invalid type for `ifn`.");
				}

				if (a.v.v_int == 0) {
					jump(insts[i].a.v_int);
				}

				break;
			default:
				break;
		}

		if (a.type.args != NULL) {
			freeTypeInfo(a.type);
		}

		if (b.type.args != NULL) {
			freeTypeInfo(b.type);
		}

		if (c.type.args != NULL) {
			freeTypeInfo(c.type);
		}
	}
}

void bc_run(bool showByteCode, bool showCount) {
	if (showByteCode) {
		bc_dump();
		return;
	}

	// Read byte code

	ObjectList o;
	o.vars = malloc(0);
	o.varsCount = 0;
	pushFrame((CallFrame){.o = &o});

	readByteCode(framesCount - 1, 0, showCount);

	popFrame();
	disposeVar(&o);
}

void bc_end() {
	// Check for stack leaks

	if (stackCount != 0) {
		fprintf(stderr, "Stack Error: Stack leak detected. Ending size `%d`.\n", stackCount);
	}

	if (locstackCount != 0) {
		fprintf(stderr, "Stack Error: Location stack leak detected. Ending size `%d`.\n", locstackCount);
	}

	// Free StackElems

	for (size_t i = 0; i < stackCount; i++) {
		if (stack[i].type.args != NULL) {
			freeTypeInfo(stack[i].type);
		}
	}

	// End

	free(funcs);
	//free(insts);
	//malloc_stats();
}