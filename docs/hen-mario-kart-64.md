<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# PS3 HEN e Mario Kart 64

Este guia pertence ao fork **Manfline/n64-ps3-hen**, baseado no
[Salsa64 de ricowp81](https://github.com/ricowp81/Salsa64).
O emulador e seus recursos principais são herdados desse projeto.
Esta preparação inicial adiciona compilação com identidade própria,
documentação em português e um perfil de configuração para Mario Kart 64.

**Situação:** a validação deste fork em um PS3 real com HEN está pendente.
Os testes no computador verificam o código portável, sem medir a velocidade
do Cell ou do RSX. O projeto original documenta Mario Kart 64 jogável em
HEN em [compatibilidad.md](compatibilidad.md); isso não substitui o teste
da nossa compilação. Os campos de vídeo por segundo do log não equivalem
necessariamente aos quadros renderizados pelo jogo.

## Gerar o instalador

No GitHub, abra **Actions > Gerar pacote PS3 HEN > Run workflow** e escolha
a branch `main`. A primeira compilação constrói o compilador para PS3 e pode
levar uma hora ou mais. O workflow roda sob demanda e entrega um artefato
com os arquivos `.pkg`, as somas SHA-256 e este perfil; ele não publica uma
release automaticamente.

Para compilar no computador, com Docker e Python 3 instalados:

```sh
tools/docker-image.sh
sh tools/build-hen.sh -j2
```

O pacote principal fica em `build/n64-ps3-hen/n64-ps3-hen.pkg`.
O título no XMB é **N64 PS3 HEN - Salsa64**, com identificador `NPSH00064`.
Ele tem identidade diferente do pacote original `SLSA00064`, mas ambos
usam **a mesma pasta de dados** `/dev_hdd0/salsa64`. Portanto compartilham
configurações e salvamentos. Faça uma cópia dos seus dados antes de testar
compilações diferentes. Uma compilação gerada ainda precisa ser testada
no console antes de ser considerada validada.

## Instalar e abrir o jogo

1. Ative o HEN no PS3. A instalação do HEN em si não faz parte deste guia.
2. Coloque o `.pkg` na raiz de um pendrive FAT32 e instale pelo Package
   Manager do XMB. Abra **N64 PS3 HEN - Salsa64** uma vez e feche.
3. Copie seu próprio dump do jogo, em `.z64`, `.v64` ou `.n64`, para a pasta
   `salsa64` na raiz do pendrive, ou para `/dev_hdd0/salsa64` por FTP ou
   gerenciador de arquivos. Arquivos compactados precisam ser extraídos.
4. Para usar o perfil fornecido, dê ao arquivo o nome `MarioKart64.z64`
   (ou `MarioKart64.v64` / `MarioKart64.n64`, conforme o formato original).
   Renomear a extensão não converte o formato do arquivo.
5. Copie `menu.cfg` e `video.cfg` de `profiles/mario-kart-64` para
   `/dev_hdd0/salsa64/`, e copie `juegos/MarioKart64.cfg` para
   `/dev_hdd0/salsa64/juegos/`. Crie essa última pasta se necessário.
   Se já usa o Salsa64, guarde suas configurações antes de substituir os arquivos.
6. Abra o emulador e selecione Mario Kart 64.

As configurações são lidas do disco interno, mesmo quando a ROM está no
pendrive. O arquivo por jogo usa o nome da ROM sem a extensão, respeitando
maiúsculas e minúsculas: `MarioKart64.z64` usa `juegos/MarioKart64.cfg`.
Se mantiver outro nome para a ROM, adapte também o nome desse `.cfg`.

## Configuração inicial e controles

O perfil usa menu em português, saída 720p, escala interna 2x, aspecto 4:3,
filtro suave, interpolação desligada e contador de velocidade ligado.
Esses valores são um ponto de partida para medição, sem promessa de FPS.
O Controller Pak fica habilitado para permitir os fantasmas de Time Trials.
O mapeamento abaixo é aplicado somente ao jogo com o nome indicado.

| Ação no jogo | DualShock 3 | Botão N64 |
|---|---|---|
| Direção | Analógico esquerdo | Analógico |
| Acelerar | X | A |
| Frear / ré | Quadrado | B |
| Usar item | L2 ou R2 | Z |
| Pular / derrapar | R1 | R |
| Pausar | START | START |
| Menu do emulador | START + SELECT | Atalho do emulador |

Use o menu do emulador para sair de uma partida. Os salvamentos normais
ficam em `/dev_hdd0/salsa64/saves`; estados do emulador ficam em
`/dev_hdd0/salsa64/states`. Valide o salvamento normal fechando e reabrindo
o jogo, além de testar save states.

## Roteiro de validação no console

- Anotar modelo do PS3, versão do sistema, versão do HEN e commit do pacote.
- Anotar região e revisão da ROM. As referências de desempenho do projeto
  original citadas aqui foram obtidas com jogos NTSC.
- Testar menu, início de corrida e uma copa completa no Grand Prix.
- Testar itens, derrapagem, áudio e entrada nos menus.
- Testar Time Trials e a persistência de um fantasma no Controller Pak.
- Testar partidas com dois e quatro controles, quando disponíveis.
- Testar salvar/carregar estado, sair e abrir outro jogo repetidamente.

Se houver erro, feche o emulador e copie
`/dev_hdd0/salsa64/salsa64.log` antes de abri-lo novamente.
O log continua com o nome original; sua primeira linha identifica o título,
o APPID e a versão deste fork. Registre o cenário e a identificação da compilação
antes de alterar configurações. Não inclua ROMs ou dados pessoais no relato.

## Próximos passos

1. Confirmar a geração do PKG e validar o primeiro início no HEN.
2. Registrar resultados de Mario Kart 64 no console.
3. Investigar e corrigir apenas os problemas reproduzidos, medindo antes
   e depois quando houver alteração de desempenho.

Não há ROM comercial, firmware ou SDK proprietário neste projeto. As
licenças e os créditos do Salsa64 continuam em [LICENSE](../LICENSE),
[CONTRIBUTING.md](../CONTRIBUTING.md) e [THIRD_PARTY.md](THIRD_PARTY.md).
