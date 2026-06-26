FROM node:22-alpine

ARG MCP_EVERYTHING_VERSION=2026.1.26

RUN npm install -g "@modelcontextprotocol/server-everything@${MCP_EVERYTHING_VERSION}"

ENV PORT=3001
EXPOSE 3001

CMD ["mcp-server-everything", "streamableHttp"]
