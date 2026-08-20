.PHONY: up down logs build token health

build:
	docker compose build

up:
	docker compose up -d

down:
	docker compose down

logs:
	docker compose logs -f relay

token:
	@openssl rand -hex 32

health:
	@curl -fsS http://127.0.0.1:8090/api/v1/health && echo
