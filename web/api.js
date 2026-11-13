"use strict";
// Shared HTTP transport. It never substitutes demonstration data on failure.
window.UCAPI = (() => {
  let token;
  async function request(path, method = "GET", body, format = "json") {
    if (method !== "GET" && !token) {
      token = (await request("/session")).token;
      if (typeof token !== "string" || !token) throw Error("Invalid session token");
    }
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 4000);
    try {
      const response = await fetch("/api/v1" + path, {
        method,
        headers:
          method === "GET" ? {} : { "Content-Type": "application/json", "X-UC-Token": token },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal,
      });
      let data;
      try {
        data = format === "text" ? await response.text() : await response.json();
      } catch {
        throw Error("Invalid JSON (HTTP " + response.status + ")");
      }
      if (!response.ok) {
        const error = Error((typeof data === "object" && data.error) || "HTTP " + response.status);
        error.httpStatus = response.status;
        throw error;
      }
      return data;
    } finally {
      clearTimeout(timeout);
    }
  }
  return { request };
})();
