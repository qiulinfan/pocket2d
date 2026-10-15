// Optional authoring frontend for the same real component as the handwritten Lua.
import { isSavedController, type SavedController } from "./SaveSchema";

interface TypedControllerState extends Component {
    health: number; dead: boolean; paused: boolean; ready: boolean; mode: string; facing: number;
    attacks: number; damage_events: number; heal_events: number;
    frame_events: number; completed_events: number; hit_events: number;
    attack_finished: boolean; trace: string; save_status: string; load_status: string;
    save_count: number; load_count: number;
    body?: Rigidbody; anim?: SpriteAnimation; sprite?: SpriteRenderer;
    OnStart(this: TypedControllerState): void;
    OnAnimationEvent(this: TypedControllerState, event: AnimationEvent): void;
    OnUpdate(this: TypedControllerState): void;
    ValidSave(this: TypedControllerState, data: unknown): data is SavedController;
    SaveGame(this: TypedControllerState): void;
    LoadGame(this: TypedControllerState): void;
}
// Module-local helpers are bundled; the engine component is explicitly global.
declare global { var TypedController: TypedControllerState; }
globalThis.TypedController = ({
    enabled: true,
    health: 3, dead: false, paused: false, ready: false, mode: "idle", facing: 1,
    attacks: 0, damage_events: 0, heal_events: 0, frame_events: 0, completed_events: 0, hit_events: 0,
    attack_finished: false, trace: "", save_status: "none", load_status: "none", save_count: 0, load_count: 0,
    OnStart(this: TypedControllerState): void {
        const player = Actor.Find("player");
        if (!player) return;
        this.body = player.GetComponent("Rigidbody");
        this.anim = player.GetComponent("SpriteAnimation");
        this.sprite = player.GetComponent("SpriteRenderer");
        this.ready = this.body !== undefined && this.anim !== undefined && this.sprite !== undefined;
    },
    OnAnimationEvent(this: TypedControllerState, event: AnimationEvent): void {
        this.trace += event.type + ":" + event.clip + ":" + event.frame + ":" + event.marker + ";";
        if (event.type === "frame") {
            this.frame_events += 1;
            if (event.marker === "hit") this.hit_events += 1;
        } else if (event.type === "completed") {
            this.completed_events += 1;
            if (event.clip === "attack") this.attack_finished = true;
        }
    },
    ValidSave(this: TypedControllerState, value: unknown): value is SavedController {
        return isSavedController(value);
    },
    SaveGame(this: TypedControllerState): void {
        if (!this.body) return;
        const p = this.body.GetPosition();
        const data = { health: this.health, position: { x: p.x, y: p.y }, facing: this.facing };
        if (!this.ValidSave(data)) { this.save_status = "invalid_game_schema"; return; }
        const result = Save.Write("main", 1, data);
        this.save_status = result.ok ? "saved" : result.code;
        if (result.ok) this.save_count += 1;
    },
    LoadGame(this: TypedControllerState): void {
        if (!this.body || !this.anim || !this.sprite) return;
        const result = Save.Read("main");
        if (!result.ok) { this.load_status = result.code; return; }
        if (result.schema_version !== 1) { this.load_status = "unsupported_game_schema"; return; }
        if (!this.ValidSave(result.data)) { this.load_status = "invalid_game_schema"; return; }
        const data = result.data;
        this.health = data.health; this.dead = this.health === 0; this.facing = data.facing;
        this.body.SetPosition(Vector2(data.position.x, data.position.y)); this.body.SetVelocity(Vector2(0, 0));
        this.sprite.scale_x = this.facing * 5; this.paused = false; this.attack_finished = false;
        this.mode = this.dead ? "dead" : "idle";
        if (this.dead) this.anim.Stop(); else this.anim.Restart("idle");
        this.load_status = "loaded"; this.load_count += 1;
    },
    OnUpdate(this: TypedControllerState): void {
        const body = this.body, anim = this.anim, sprite = this.sprite;
        if (!this.ready || !body || !anim || !sprite) return;
        if (Input.GetKeyDown("f9")) { this.LoadGame(); return; }
        if (Input.GetKeyDown("f5")) { body.SetVelocity(Vector2(0, 0)); this.SaveGame(); return; }
        if (Input.GetKeyDown("h") && this.health > 0) {
            this.health = Math.max(0, this.health - 1); this.damage_events += 1;
        }
        if (this.health === 0) {
            if (!this.dead) anim.Stop();
            this.dead = true; this.mode = "dead"; body.SetVelocity(Vector2(0, 0)); return;
        }
        if (Input.GetKeyDown("g") && this.health < 3) {
            this.health = Math.min(3, this.health + 1); this.heal_events += 1;
        }
        if (Input.GetKeyDown("p")) { this.paused = true; anim.Pause(); }
        if (Input.GetKeyDown("r")) { this.paused = false; anim.Resume(); }
        if (this.paused) { body.SetVelocity(Vector2(0, 0)); return; }
        let x = 0, y = 0;
        if (Input.GetKey("right")) x += 1;
        if (Input.GetKey("left")) x -= 1;
        if (Input.GetKey("down")) y += 1;
        if (Input.GetKey("up")) y -= 1;
        if (x !== 0 && y !== 0) { x /= Math.sqrt(2); y /= Math.sqrt(2); }
        body.SetVelocity(Vector2(x * 2, y * 2));
        if (x !== 0) { this.facing = x < 0 ? -1 : 1; sprite.scale_x = this.facing * 5; }
        if (this.mode === "attack") {
            if (!this.attack_finished) return;
            this.attack_finished = false; this.mode = "idle";
        }
        if (Input.GetKeyDown("space")) {
            this.attacks += 1; this.mode = "attack"; anim.Restart("attack"); return;
        }
        this.mode = (x !== 0 || y !== 0) ? "walk" : "idle";
        anim.Play(this.mode);
    },
} satisfies Omit<TypedControllerState, "actor" | "key">) as TypedControllerState;
