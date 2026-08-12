import {json} from "./security";

export const INTERNAL_API_HEADER = "x-mdkr-internal-api";
export const INTERNAL_API_VERSION = "1";

export function internalRequest(init: RequestInit = {}): RequestInit {
  const headers = new Headers(init.headers);
  headers.set(INTERNAL_API_HEADER, INTERNAL_API_VERSION);
  return {...init, headers};
}

export function rejectUnsupportedInternalApi(request: Request): Response | null {
  const version = request.headers.get(INTERNAL_API_HEADER);
  // Unversioned is the frozen pre-v1 contract and must remain readable while an
  // older Worker can still reach a newly deployed Durable Object. Removing it
  // is a separate contract-phase deployment, never a same-release cleanup.
  if (version === null || version === INTERNAL_API_VERSION) return null;
  return json({error: "protocol_update_required"}, 426);
}
