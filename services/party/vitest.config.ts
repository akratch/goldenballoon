import {defineConfig} from "vitest/config";
import {cloudflareTest} from "@cloudflare/vitest-pool-workers";

export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: {configPath: "./wrangler.jsonc"},
    miniflare: {
      bindings: {
        PARTY_HMAC_KEY: "test-only-0123456789abcdef0123456789abcdef",
      },
    },
  })],
});
