/* ============================================================
 * login.js - Login e cadastro de conta (compartilhado entre
 * index.html e chat.html)
 *
 * Guarda a sessao no localStorage do navegador (so nome + email,
 * nunca a senha). Isso e por pagina/navegador: se a pessoa abrir
 * em outro computador, precisa logar de novo - mas a CONTA em si
 * fica salva no usuarios.txt compartilhado, entao funciona em
 * qualquer maquina que rode o mesmo server.c.
 * ============================================================ */

function escaparHtmlLogin(texto) {
  const div = document.createElement('div');
  div.textContent = texto;
  return div.innerHTML;
}

function usuarioLogado() {
  const dados = localStorage.getItem('padaria-usuario');
  return dados ? JSON.parse(dados) : null;
}

function salvarUsuarioLogado(nome, email) {
  localStorage.setItem('padaria-usuario', JSON.stringify({ nome, email }));
}

function sairDaConta() {
  localStorage.removeItem('padaria-usuario');
  location.reload();
}

/* Validacao de CPF de verdade: confere os dois digitos verificadores
   (mesmo algoritmo usado no servidor), nao so o formato. */
function validarCPF(cpfBruto) {
  const cpf = (cpfBruto || '').replace(/\D/g, '');
  if (cpf.length !== 11) return false;
  if (/^(\d)\1{10}$/.test(cpf)) return false;

  for (let parte = 0; parte < 2; parte++) {
    const tamCalculo = 9 + parte;
    let soma = 0, peso = tamCalculo + 1;
    for (let i = 0; i < tamCalculo; i++) {
      soma += parseInt(cpf[i], 10) * peso;
      peso--;
    }
    let resto = (soma * 10) % 11;
    if (resto === 10) resto = 0;
    if (resto !== parseInt(cpf[tamCalculo], 10)) return false;
  }
  return true;
}

function formatarCPFEnquantoDigita(campo) {
  campo.addEventListener('input', () => {
    let v = campo.value.replace(/\D/g, '').slice(0, 11);
    v = v.replace(/(\d{3})(\d)/, '$1.$2');
    v = v.replace(/(\d{3})(\d)/, '$1.$2');
    v = v.replace(/(\d{3})(\d{1,2})$/, '$1-$2');
    campo.value = v;
  });
}

function formatarTelefoneEnquantoDigita(campo) {
  campo.addEventListener('input', () => {
    let v = campo.value.replace(/\D/g, '').slice(0, 11);
    if (v.length > 10) {
      v = v.replace(/(\d{2})(\d{5})(\d{0,4})/, '($1) $2-$3');
    } else if (v.length > 5) {
      v = v.replace(/(\d{2})(\d{4})(\d{0,4})/, '($1) $2-$3');
    } else if (v.length > 2) {
      v = v.replace(/(\d{2})(\d{0,5})/, '($1) $2');
    }
    campo.value = v.replace(/-$/, '').replace(/\)\s$/, ')');
  });
}

/* ---------- Controle do modal de login ---------- */

let aoLogarComSucesso = null; // callback definido por quem chamou iniciarLogin()

/* Se ja existe sessao salva, chama o callback direto. Senao, abre o modal
   (fundo desfocado + caixa central) e so chama o callback depois do login. */
function iniciarLogin(callbackSucesso) {
  aoLogarComSucesso = callbackSucesso;
  const usuario = usuarioLogado();
  if (usuario) {
    if (aoLogarComSucesso) aoLogarComSucesso(usuario);
    return;
  }
  const modal = document.getElementById('modal-login');
  if (modal) modal.style.display = 'flex';
}

function fecharModalLogin() {
  const modal = document.getElementById('modal-login');
  if (modal) modal.style.display = 'none';
}

function alternarModoCadastro(mostrarCadastro) {
  document.getElementById('form-login').style.display = mostrarCadastro ? 'none' : 'flex';
  document.getElementById('form-cadastro').style.display = mostrarCadastro ? 'flex' : 'none';
  document.getElementById('aba-entrar').classList.toggle('aba-ativa', !mostrarCadastro);
  document.getElementById('aba-criar').classList.toggle('aba-ativa', mostrarCadastro);
}

async function enviarLogin(evento) {
  evento.preventDefault();
  const email = document.getElementById('login-email').value.trim();
  const senha = document.getElementById('login-senha').value;
  const erroEl = document.getElementById('login-erro');
  erroEl.textContent = '';

  try {
    const resposta = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `email=${encodeURIComponent(email)}&senha=${encodeURIComponent(senha)}`
    });
    const texto = await resposta.text();
    const separador = texto.indexOf('|||');
    const status = separador > -1 ? texto.slice(0, separador) : texto;
    const valor = separador > -1 ? texto.slice(separador + 3) : '';

    if (status === 'OK') {
      salvarUsuarioLogado(valor, email);
      fecharModalLogin();
      atualizarStatusLogin();
      if (aoLogarComSucesso) aoLogarComSucesso({ nome: valor, email });
    } else {
      erroEl.textContent = valor || 'Nao foi possivel entrar.';
    }
  } catch (erro) {
    erroEl.textContent = 'Erro ao falar com o servidor.';
  }
}

async function enviarCadastro(evento) {
  evento.preventDefault();
  const nome = document.getElementById('cadastro-nome').value.trim();
  const email = document.getElementById('cadastro-email').value.trim();
  const cpf = document.getElementById('cadastro-cpf').value.trim();
  const telefone = document.getElementById('cadastro-telefone').value.trim();
  const senha = document.getElementById('cadastro-senha').value;
  const erroEl = document.getElementById('cadastro-erro');
  erroEl.textContent = '';

  if (!validarCPF(cpf)) {
    erroEl.textContent = 'CPF invalido. Confira os numeros digitados.';
    return;
  }
  if (senha.length < 4) {
    erroEl.textContent = 'A senha precisa ter pelo menos 4 caracteres.';
    return;
  }

  try {
    const resposta = await fetch('/api/cadastrar', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `nome=${encodeURIComponent(nome)}&email=${encodeURIComponent(email)}`
          + `&cpf=${encodeURIComponent(cpf)}&telefone=${encodeURIComponent(telefone)}`
          + `&senha=${encodeURIComponent(senha)}`
    });
    const texto = await resposta.text();
    const separador = texto.indexOf('|||');
    const status = separador > -1 ? texto.slice(0, separador) : texto;
    const valor = separador > -1 ? texto.slice(separador + 3) : '';

    if (status === 'OK') {
      salvarUsuarioLogado(valor, email);
      fecharModalLogin();
      atualizarStatusLogin();
      if (aoLogarComSucesso) aoLogarComSucesso({ nome: valor, email });
    } else {
      erroEl.textContent = valor || 'Nao foi possivel criar a conta.';
    }
  } catch (erro) {
    erroEl.textContent = 'Erro ao falar com o servidor.';
  }
}

function atualizarStatusLogin() {
  const el = document.getElementById('status-login');
  if (!el) return;
  const usuario = usuarioLogado();
  el.innerHTML = usuario
    ? `<span class="status-login-nome">Olá, ${escaparHtmlLogin(usuario.nome)}</span>
       <button type="button" class="botao-sair" onclick="sairDaConta()">
         <svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
           <path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"></path>
           <polyline points="16 17 21 12 16 7"></polyline>
           <line x1="21" y1="12" x2="9" y2="12"></line>
         </svg>
         Sair
       </button>`
    : '';
}

document.addEventListener('DOMContentLoaded', () => {
  const campoCpf = document.getElementById('cadastro-cpf');
  if (campoCpf) formatarCPFEnquantoDigita(campoCpf);

  const campoTel = document.getElementById('cadastro-telefone');
  if (campoTel) formatarTelefoneEnquantoDigita(campoTel);

  atualizarStatusLogin();
});