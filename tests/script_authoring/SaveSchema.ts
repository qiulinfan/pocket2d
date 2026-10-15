// Runtime validation stays mandatory even with static authoring types.
export interface SavedController {
    health: number;
    position: { x: number; y: number };
    facing: number;
}

export function isSavedController(this: void, value: unknown): value is SavedController {
    if (typeof value !== "object" || value === null) return false;
    const data = value as Record<string, unknown>;
    const keys = Object.keys(data);
    for (const key of keys) if (key !== "health" && key !== "position" && key !== "facing") return false;
    if (keys.length !== 3 || typeof data.health !== "number" || data.health !== Math.floor(data.health)
        || data.health < 0 || data.health > 3 || (data.facing !== 1 && data.facing !== -1)
        || typeof data.position !== "object" || data.position === null) return false;
    const position = data.position as Record<string, unknown>;
    const positionKeys = Object.keys(position);
    for (const key of positionKeys) if (key !== "x" && key !== "y") return false;
    if (positionKeys.length !== 2) return false;
    for (const coordinate of [position.x, position.y]) {
        if (typeof coordinate !== "number" || coordinate !== coordinate || Math.abs(coordinate) > 1000) return false;
    }
    return true;
}
